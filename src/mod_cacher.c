/*
 * mod_cacher - JSON-configurable, cookie-aware disk caching for Apache httpd.
 *
 * This file owns module registration and request-lifecycle wiring only;
 * the actual work is delegated to single-responsibility units:
 *   - cacher_config.c: directives, per-dir/per-server config, rules cache
 *   - cacher_rules.c:  JSON rule parsing and matching (Apache-independent)
 *   - cacher_cache.c:  disk cache read/write
 *   - cacher_util.c:   cookie header parsing
 */

#include "cacher_config.h"
#include "cacher_cache.h"
#include "cacher_util.h"

#include "http_log.h"
#include "http_protocol.h"

static int cacher_quick_handler(request_rec *r, int lookup_uri)
{
    cacher_dir_conf *dconf;
    cacher_svr_conf *sconf;
    const cacher_ruleset *rs;
    const cacher_rule *rule;
    apr_file_t *body;
    int status;
    apr_off_t length;

    (void) lookup_uri;

    dconf = ap_get_module_config(r->per_dir_config, &cacher_module);
    if (!dconf || dconf->enabled != 1) {
        return DECLINED;
    }

    rs = cacher_config_get_ruleset(r, dconf);
    if (!rs) {
        return DECLINED;
    }

    rule = cacher_ruleset_match(rs, r->parsed_uri.path ? r->parsed_uri.path : r->uri, r->method);
    if (!rule) {
        return DECLINED;
    }

    if (cacher_request_bypasses(r, rule)) {
        ap_log_rerror(APLOG_MARK, APLOG_TRACE1, 0, r,
                      "cacher: %s %s bypassed (matching cookie present)", r->method, r->uri);
        return DECLINED;
    }

    sconf = ap_get_module_config(r->server->module_config, &cacher_module);

    body = cacher_cache_lookup(r, sconf ? sconf->cache_root : NULL, rule, &status, &length);
    if (body) {
        apr_bucket_brigade *bb;

        r->status = status;
        ap_set_content_length(r, length);

        bb = apr_brigade_create(r->pool, r->connection->bucket_alloc);
        apr_brigade_insert_file(bb, body, 0, length, r->pool);
        APR_BRIGADE_INSERT_TAIL(bb, apr_bucket_eos_create(r->connection->bucket_alloc));

        ap_log_rerror(APLOG_MARK, APLOG_TRACE1, 0, r,
                      "cacher: %s %s served from cache", r->method, r->uri);

        ap_pass_brigade(r->output_filters, bb);
        return OK;
    }

    /* MISS: capture this response to disk as it's generated, then let
     * normal request processing run. */
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
    ap_hook_quick_handler(cacher_quick_handler, NULL, NULL, APR_HOOK_FIRST);
}

AP_DECLARE_MODULE(cacher) = {
    STANDARD20_MODULE_STUFF,
    cacher_create_dir_config,
    cacher_merge_dir_config,
    cacher_create_server_config,
    cacher_merge_server_config,
    cacher_cmds,
    cacher_register_hooks
};
