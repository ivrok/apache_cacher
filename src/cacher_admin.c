#include "cacher_admin.h"

#include <string.h>

#include "apr_file_info.h"
#include "apr_file_io.h"
#include "apr_strings.h"
#include "http_log.h"
#include "http_protocol.h"

#include "cacher_cache.h"
#include "cacher_config.h"
#include "cacher_rules.h"
#include "cacher_util.h"

APLOG_USE_MODULE(cacher);

typedef struct {
    request_rec *r;
    const char *pattern;   /* NULL for list/reset */
    int matched;
    apr_off_t bytes;
    int delete_matches;
    int print_rows;
} admin_ctx;

/* Deletes an entry's body then its header: a reader only trusts an entry
 * once the header exists, so the header is the last thing to go. */
static void delete_entry(request_rec *r, const char *header_path)
{
    char *body_path = apr_pstrndup(r->pool, header_path,
                                    strlen(header_path) - strlen(".header"));
    body_path = apr_pstrcat(r->pool, body_path, ".body", NULL);

    apr_file_remove(body_path, r->pool);
    apr_file_remove(header_path, r->pool);
}

static apr_off_t body_size(request_rec *r, const char *header_path)
{
    apr_finfo_t finfo;
    char *body_path = apr_pstrndup(r->pool, header_path,
                                    strlen(header_path) - strlen(".header"));
    body_path = apr_pstrcat(r->pool, body_path, ".body", NULL);

    if (apr_stat(&finfo, body_path, APR_FINFO_SIZE, r->pool) != APR_SUCCESS) {
        return 0;
    }
    return finfo.size;
}

static void visit_entry(admin_ctx *ctx, const char *header_path)
{
    request_rec *r = ctx->r;
    cacher_entry_meta meta;
    apr_int64_t left;

    if (cacher_cache_read_meta(r->pool, header_path, &meta, NULL) != 0) {
        return;
    }

    /* Never touch another site's entries: the cache root is shared by every
     * vhost, and this endpoint may have been enabled from a single site's
     * .htaccess. */
    if (!cacher_streq_ci(meta.host, r->hostname ? r->hostname : "")) {
        return;
    }

    if (ctx->pattern
        && !cacher_glob_match(ctx->pattern, meta.url, 0)
        && !cacher_glob_match(ctx->pattern,
                              apr_pstrcat(r->pool, meta.host, meta.url, NULL), 0)) {
        return;
    }

    ctx->matched++;
    ctx->bytes += body_size(r, header_path);

    if (ctx->print_rows) {
        left = meta.expires - (apr_int64_t) apr_time_sec(apr_time_now());
        ap_rprintf(r, "%-6s %3d %10" APR_OFF_T_FMT "  %8s  %s%s\n",
                   meta.method, meta.status, body_size(r, header_path),
                   left > 0 ? apr_psprintf(r->pool, "%" APR_INT64_T_FMT "s", left) : "STALE",
                   meta.host, meta.url);
    }

    if (ctx->delete_matches) {
        delete_entry(r, header_path);
    }
}

/* Walks the sharded cache tree, calling visit_entry for every .header. */
static void walk_cache(admin_ctx *ctx, const char *dir)
{
    apr_dir_t *d;
    apr_finfo_t finfo;

    if (apr_dir_open(&d, dir, ctx->r->pool) != APR_SUCCESS) {
        return;
    }

    while (apr_dir_read(&finfo, APR_FINFO_NAME | APR_FINFO_TYPE, d) == APR_SUCCESS) {
        const char *child;

        if (!finfo.name || finfo.name[0] == '.') {
            continue;
        }
        child = apr_pstrcat(ctx->r->pool, dir, "/", finfo.name, NULL);

        if (finfo.filetype == APR_DIR) {
            walk_cache(ctx, child);
        } else if (strstr(finfo.name, ".header") && !strstr(finfo.name, ".XXXXXX")) {
            visit_entry(ctx, child);
        }
    }

    apr_dir_close(d);
}

static int usage(request_rec *r, const char *base)
{
    ap_set_content_type(r, "text/plain; charset=utf-8");
    ap_rprintf(r,
               "Cacher admin\n\n"
               "  GET %s/list                 list every cached entry\n"
               "  GET %s/purge/<path-glob>    purge entries matching a glob\n"
               "  GET %s/full-reset           purge everything\n\n"
               "Examples:\n"
               "  %s/purge/blog/*\n"
               "  %s/purge/about/\n",
               base, base, base, base, base);
    return OK;
}

int cacher_admin_handle(request_rec *r, const char *cache_root, const char *action)
{
    admin_ctx ctx;

    if (!cache_root) {
        ap_set_content_type(r, "text/plain; charset=utf-8");
        ap_rputs("CacherCacheRoot is not configured - nothing to manage.\n", r);
        return OK;
    }

    memset(&ctx, 0, sizeof(ctx));
    ctx.r = r;

    if (!action || !*action || strcmp(action, "/") == 0) {
        return usage(r, r->uri);
    }

    if (strcmp(action, "/list") == 0) {
        ctx.print_rows = 1;
        ap_set_content_type(r, "text/plain; charset=utf-8");
        ap_rprintf(r, "%-6s %3s %10s  %8s  %s\n", "METHOD", "ST", "SIZE", "EXPIRES", "URL");
        walk_cache(&ctx, cache_root);
        ap_rprintf(r, "\n%d entr%s, %" APR_OFF_T_FMT " bytes\n",
                   ctx.matched, ctx.matched == 1 ? "y" : "ies", ctx.bytes);
        return OK;
    }

    if (strcmp(action, "/full-reset") == 0) {
        ctx.delete_matches = 1;
        walk_cache(&ctx, cache_root);
        ap_log_rerror(APLOG_MARK, APLOG_NOTICE, 0, r,
                      "cacher: full-reset via admin endpoint removed %d entries for %s",
                      ctx.matched, r->hostname ? r->hostname : "-");
        ap_set_content_type(r, "text/plain; charset=utf-8");
        ap_rprintf(r, "full-reset: removed %d entr%s\n",
                   ctx.matched, ctx.matched == 1 ? "y" : "ies");
        return OK;
    }

    if (strncmp(action, "/purge", 6) == 0) {
        const char *pattern = action + 6;

        if (!*pattern || strcmp(pattern, "/") == 0) {
            ap_set_content_type(r, "text/plain; charset=utf-8");
            ap_rputs("purge: no pattern given, e.g. /purge/blog/*\n", r);
            return HTTP_BAD_REQUEST;
        }

        ctx.pattern = pattern;
        ctx.delete_matches = 1;
        walk_cache(&ctx, cache_root);

        ap_log_rerror(APLOG_MARK, APLOG_NOTICE, 0, r,
                      "cacher: purge '%s' via admin endpoint removed %d entries for %s",
                      pattern, ctx.matched, r->hostname ? r->hostname : "-");

        ap_set_content_type(r, "text/plain; charset=utf-8");
        ap_rprintf(r, "purge %s: removed %d entr%s\n",
                   pattern, ctx.matched, ctx.matched == 1 ? "y" : "ies");
        return ctx.matched ? OK : HTTP_NOT_FOUND;
    }

    return usage(r, r->uri);
}
