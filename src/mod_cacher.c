/*
 * mod_cacher - JSON-configurable, cookie-aware disk caching for Apache httpd.
 *
 * This file owns module registration and request-lifecycle wiring only;
 * the actual work is delegated to single-responsibility units:
 *   - cacher_config.c: directives, per-dir/per-server config, rules cache
 *   - cacher_rules.c:  JSON rule parsing and matching (Apache-independent)
 *   - cacher_cache.c:  disk cache read/write
 *   - cacher_util.c:   cookie header parsing
 *   - cacher_admin.c:  list/purge/full-reset HTTP endpoints
 */

#include "cacher_config.h"
#include "cacher_admin.h"
#include "cacher_cache.h"
#include "cacher_util.h"

#include <string.h>

#include "http_log.h"
#include "http_protocol.h"

/*
 * Runs as a content handler, NOT as a quick_handler.
 *
 * quick_handler is the obvious hook for a URI-keyed cache and is what
 * mod_cache uses - but it fires before ap_process_request_internal(), so
 * directory_walk has not run and r->per_dir_config holds only server
 * defaults. Any CacherEnable/CacherRules set in .htaccess is therefore
 * invisible there, which is precisely why mod_cache's own CacheEnable is
 * server-config-only. Since per-directory .htaccess rules are the whole
 * point of this module, the read path lives here instead.
 *
 * The trade-off is deliberate and favourable: a handler runs after the
 * authentication and authorisation phases, so a cache hit can no longer
 * be served to a request that would have failed auth. Cache hits still
 * skip the actual content generator (PHP, etc.), which is where the cost
 * of a dynamic request really lies.
 */
static int cacher_handler(request_rec *r)
{
    cacher_dir_conf *dconf;
    cacher_svr_conf *sconf;
    const cacher_ruleset *rs;
    const cacher_rule *rule;
    request_rec *orig;
    apr_file_t *body;
    int status;
    apr_off_t length;

    /* Subrequests (SSI includes and friends) are fragments of another
     * response, not independently addressable resources - never cache or
     * serve them in their own right. */
    if (r->main) {
        return DECLINED;
    }

    /* Admin endpoints are checked before anything else: they live at their
     * own URL and must work regardless of whether that path sits inside a
     * Cacher-enabled directory. CacherAdminPath is per-directory, so each
     * site can enable them for itself - including from .htaccess - and
     * cacher_admin.c scopes every operation to the requesting host so one
     * site can never list or purge another's entries. */
    sconf = ap_get_module_config(r->server->module_config, &cacher_module);
    dconf = ap_get_module_config(r->per_dir_config, &cacher_module);

    if (dconf && dconf->admin_path) {
        apr_size_t admin_len = strlen(dconf->admin_path);
        request_rec *top = cacher_original_request(r);

        if (strncmp(r->uri, dconf->admin_path, admin_len) == 0
            && (r->uri[admin_len] == '\0' || r->uri[admin_len] == '/')) {

            /*
             * Web administration is off by default (no CacherAdminPath), and
             * turning it on should not silently mean "open to the internet".
             * r->user is set only when Apache actually authenticated somebody,
             * so requiring it here makes an unauthenticated admin endpoint a
             * deliberate, written-down decision rather than an oversight.
             *
             * Note this asks specifically for an authenticated *user*: an
             * IP-allowlisted setup (Require ip) leaves r->user NULL, so those
             * deployments also set CacherAdminRequireUser Off - the gate is
             * still Apache's, this directive only records that the module was
             * told not to insist on a user.
             */
            if (dconf->admin_require_user != 0 && !r->user) {
                ap_log_rerror(APLOG_MARK, APLOG_WARNING, 0, r,
                              "cacher: refusing admin request for %s - no authenticated "
                              "user. Guard the path with AuthType/Require valid-user, or "
                              "set 'CacherAdminRequireUser Off' to allow unauthenticated "
                              "access (appropriate only where anyone resetting the cache "
                              "is acceptable, e.g. staging).", r->uri);
                return HTTP_FORBIDDEN;
            }

            return cacher_admin_handle(r, sconf ? sconf->cache_root : NULL,
                                        r->uri + admin_len);
        }

        /*
         * The client asked for the admin path, but a rewrite redirected the
         * request elsewhere before we ran. Matching on the original URI here
         * would make the endpoint work - and would be a security bug:
         * <Location> is matched against the POST-rewrite URI, so any
         * Require/auth guarding the admin path no longer applies to this
         * request. Serving it would run cache administration with its access
         * control silently switched off. Refuse, and say how to fix it.
         */
        if (top != r
            && strncmp(top->uri, dconf->admin_path, admin_len) == 0
            && (top->uri[admin_len] == '\0' || top->uri[admin_len] == '/')) {
            ap_log_rerror(APLOG_MARK, APLOG_WARNING, 0, r,
                          "cacher: '%s' was rewritten to '%s' before reaching the "
                          "handler, so any <Location %s> access control no longer "
                          "applies - refusing to serve the admin endpoint. Exempt "
                          "it from the front-controller rewrite, e.g. "
                          "'RewriteRule ^%s - [END]' above the application's rules.",
                          top->uri, r->uri, dconf->admin_path, dconf->admin_path + 1);
            return DECLINED;
        }
    }

    if (!dconf || dconf->enabled != 1) {
        /* Deliberately silent: this hook runs for every request on the
         * server, and the overwhelming majority are not Cacher-enabled. */
        return DECLINED;
    }

    rs = cacher_config_get_ruleset(r, dconf);
    if (!rs) {
        ap_log_rerror(APLOG_MARK, APLOG_TRACE1, 0, r,
                      "cacher: %s enabled here but no usable rules "
                      "(CacherRules/CacherRulesFile missing or failed to parse)", r->uri);
        return DECLINED;
    }

    /* Match on what the client asked for, not the post-rewrite URI - see
     * cacher_original_request(). Without this, a front-controller rewrite
     * turns every request into "/index.php" and every path exclusion is
     * silently bypassed by the redirected request. */
    orig = cacher_original_request(r);

    rule = cacher_ruleset_match(rs, orig->parsed_uri.path ? orig->parsed_uri.path : orig->uri,
                                 orig->args, r->method);
    if (!rule) {
        ap_log_rerror(APLOG_MARK, APLOG_TRACE1, 0, r,
                      "cacher: %s %s matched no rule", r->method, orig->uri);
        return DECLINED;
    }

    if (!rule->enabled) {
        ap_log_rerror(APLOG_MARK, APLOG_TRACE1, 0, r,
                      "cacher: %s %s matched an exclusion rule - not cacheable",
                      r->method, orig->uri);
        return DECLINED;
    }

    if (cacher_request_bypasses(r, rule)) {
        ap_log_rerror(APLOG_MARK, APLOG_TRACE1, 0, r,
                      "cacher: %s %s bypassed (matching cookie present)", r->method, orig->uri);
        return DECLINED;
    }

    body = cacher_cache_lookup(r, sconf ? sconf->cache_root : NULL, rule, &status, &length);
    if (body) {
        apr_bucket_brigade *bb;

        r->status = status;
        ap_set_content_length(r, length);

        bb = apr_brigade_create(r->pool, r->connection->bucket_alloc);
        apr_brigade_insert_file(bb, body, 0, length, r->pool);
        APR_BRIGADE_INSERT_TAIL(bb, apr_bucket_eos_create(r->connection->bucket_alloc));

        ap_log_rerror(APLOG_MARK, APLOG_TRACE1, 0, r,
                      "cacher: %s %s served from cache", r->method, orig->uri);

        ap_pass_brigade(r->output_filters, bb);
        return OK;
    }

    /* MISS: attach the capture filter now, then DECLINE so the real
     * content handler (PHP, static file, ...) generates the response.
     * Adding an output filter here is safe - filters only need to be in
     * place before content starts flowing, and this hook runs first. */
    cacher_cache_insert_filter(r, sconf ? sconf->cache_root : NULL, rule);

    return DECLINED;
}

static int cacher_post_config(apr_pool_t *pconf, apr_pool_t *plog,
                               apr_pool_t *ptemp, server_rec *s)
{
    (void) plog;
    (void) ptemp;
    cacher_cache_ensure_root(pconf, s);
    return OK;
}

static void cacher_register_hooks(apr_pool_t *p)
{
    cacher_cache_register_filter(p);
    ap_hook_post_config(cacher_post_config, NULL, NULL, APR_HOOK_MIDDLE);
    ap_hook_child_init(cacher_config_child_init, NULL, NULL, APR_HOOK_MIDDLE);
    ap_hook_handler(cacher_handler, NULL, NULL, APR_HOOK_FIRST);
}

AP_DECLARE_MODULE(cacher) = {
    STANDARD20_MODULE_STUFF,
    cacher_create_dir_config,
    cacher_merge_dir_config,
    cacher_create_server_config,
    cacher_merge_server_config,
    cacher_cmds,
    cacher_register_hooks,
    AP_MODULE_FLAG_NONE
};
