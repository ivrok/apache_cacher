#include "cacher_cache.h"

#include <string.h>

#include "apr_file_info.h"
#include "apr_file_io.h"
#include "apr_md5.h"
#include "apr_strings.h"
#include "http_log.h"
#include "http_protocol.h"
#include "util_filter.h"

#include "cacher_config.h"
#include "cacher_util.h"

/* See the note in cacher_config.c - required for "LogLevel cacher:trace1"
 * to apply to messages logged from this file. */
APLOG_USE_MODULE(cacher);

/* On-disk header file layout: this fixed-size struct, followed by a raw
 * "Name: value\r\n" block terminated by a blank line. Not portable across
 * architectures/endianness by design - this is a same-machine local cache,
 * not a transfer format. */
#define CACHER_CACHE_MAGIC 0x43414331u /* "CAC1" */

typedef struct {
    apr_uint32_t magic;
    apr_time_t expires;
    int status;
} cacher_cache_meta;

/* Per-request state for the CACHER_OUT output filter, allocated once at
 * insertion time (cacher_cache_insert_filter) and threaded through every
 * subsequent invocation of the filter via f->ctx. */
typedef struct {
    const cacher_rule *rule;
    char *header_path;
    char *body_path;
    char *tmp_body_path;
    apr_file_t *tmp_body_file;
    apr_pool_t *pool;
    int started;
    int cacheable;
} cacher_out_ctx;

static apr_status_t cacher_output_filter(ap_filter_t *f, apr_bucket_brigade *bb);

static void hex_encode_md5(const unsigned char digest[APR_MD5_DIGESTSIZE], char *out /* 33 bytes */)
{
    static const char hex[] = "0123456789abcdef";
    int i;

    for (i = 0; i < APR_MD5_DIGESTSIZE; i++) {
        out[i * 2] = hex[(digest[i] >> 4) & 0xF];
        out[i * 2 + 1] = hex[digest[i] & 0xF];
    }
    out[APR_MD5_DIGESTSIZE * 2] = '\0';
}

/* Builds the canonical string this request's cache key is hashed from:
 * method, host, path+query, then each of rule->vary's header/cookie
 * values in order. */
static char *build_cache_key_string(request_rec *r, const cacher_rule *rule)
{
    /* Key on the client-visible URI, not r->uri: after a front-controller
     * rewrite every page is "/index.php", which would collapse the whole
     * site into one cache entry. */
    request_rec *orig = cacher_original_request(r);
    char *key;
    int i;

    key = apr_pstrcat(r->pool,
                       r->method, "\n",
                       r->hostname ? r->hostname : "", "\n",
                       orig->uri, orig->args ? "?" : "", orig->args ? orig->args : "",
                       NULL);

    if (rule->vary) {
        for (i = 0; rule->vary[i]; i++) {
            const char *entry = rule->vary[i];
            const char *value;

            if (strncmp(entry, "cookie:", 7) == 0) {
                value = cacher_get_cookie(r, entry + 7);
            } else {
                value = apr_table_get(r->headers_in, entry);
            }
            key = apr_pstrcat(r->pool, key, "\n", entry, "=", value ? value : "", NULL);
        }
    }

    return key;
}

/* Resolves the sharded on-disk path pair for this request under `rule`.
 * Returns -1 (no paths set) if cache_root is unset. */
static int cache_key_paths(request_rec *r, const char *cache_root, const cacher_rule *rule,
                            char **header_path, char **body_path)
{
    char *key_str;
    unsigned char digest[APR_MD5_DIGESTSIZE];
    char hex[APR_MD5_DIGESTSIZE * 2 + 1];
    char *shard_dir;

    if (!cache_root) {
        return -1;
    }

    key_str = build_cache_key_string(r, rule);
    apr_md5(digest, key_str, strlen(key_str));
    hex_encode_md5(digest, hex);

    shard_dir = apr_psprintf(r->pool, "%s/%.2s/%.2s", cache_root, hex, hex + 2);
    *header_path = apr_psprintf(r->pool, "%s/%s.header", shard_dir, hex);
    *body_path = apr_psprintf(r->pool, "%s/%s.body", shard_dir, hex);

    return 0;
}

void cacher_cache_register_filter(apr_pool_t *p)
{
    (void) p;
    /* AP_FTYPE_CONTENT_SET (same type mod_cache uses for CACHE_SAVE), not
     * AP_FTYPE_PROTOCOL: ap_http_header_filter is itself a PROTOCOL filter,
     * and at that type we could be ordered after it and end up capturing the
     * serialized HTTP header block into the cached body. CONTENT_SET keeps
     * us upstream of header serialization, seeing body bytes only. */
    ap_register_output_filter(CACHER_OUTPUT_FILTER_NAME, cacher_output_filter, NULL, AP_FTYPE_CONTENT_SET);
}

int cacher_cache_ensure_root(apr_pool_t *p, server_rec *base_s)
{
    server_rec *s;
    int ok = 0;

    for (s = base_s; s; s = s->next) {
        cacher_svr_conf *conf = ap_get_module_config(s->module_config, &cacher_module);

        if (conf && conf->cache_root) {
            if (apr_dir_make_recursive(conf->cache_root, APR_FPROT_OS_DEFAULT, p) != APR_SUCCESS) {
                ap_log_error(APLOG_MARK, APLOG_ERR, 0, s,
                             "cacher: could not create CacherCacheRoot '%s'", conf->cache_root);
                ok = -1;
            }
        }
    }

    return ok;
}

/*
 * Headers excluded when capturing (write path) or replaying (read path) a
 * cached response:
 *  - Connection/Keep-Alive/Transfer-Encoding/Content-Length describe this
 *    specific connection/transfer, not the cached representation.
 *  - Set-Cookie is specific to the single original requester (e.g. a
 *    session/auth cookie); replaying it to every later visitor of a
 *    shared cache entry would leak one user's cookie to everyone else.
 */
static int is_uncacheable_header(const char *name)
{
    return cacher_header_name_is(name, "Connection")
        || cacher_header_name_is(name, "Keep-Alive")
        || cacher_header_name_is(name, "Transfer-Encoding")
        || cacher_header_name_is(name, "Content-Length")
        || cacher_header_name_is(name, "Set-Cookie");
}

typedef struct {
    char *text;
    apr_pool_t *pool;
} header_dump_ctx;

static int dump_header_cb(void *rec, const char *key, const char *value)
{
    header_dump_ctx *ctx = rec;

    if (!is_uncacheable_header(key)) {
        ctx->text = apr_pstrcat(ctx->pool, ctx->text, key, ": ", value, "\r\n", NULL);
    }
    return 1;
}

/* Writes meta + header block + renames body and header into place. Called
 * once, when the EOS bucket for a cacheable response is seen. Body length
 * isn't stored in the metadata - it's simply stat()'d off the body file
 * again at read time, in cacher_cache_lookup(). */
static void finalize_cache_entry(request_rec *r, cacher_out_ctx *ctx)
{
    header_dump_ctx hctx;
    char *header_text;
    char *tmp_header_path;
    apr_file_t *hf;
    cacher_cache_meta meta;
    apr_status_t rv;

    apr_file_close(ctx->tmp_body_file);

    hctx.text = "";
    hctx.pool = r->pool;
    apr_table_do(dump_header_cb, &hctx, r->headers_out, NULL);
    header_text = apr_pstrcat(r->pool, hctx.text, "\r\n", NULL);

    meta.magic = CACHER_CACHE_MAGIC;
    meta.status = r->status;
    meta.expires = apr_time_now() + apr_time_from_sec(ctx->rule->ttl_seconds);

    tmp_header_path = apr_pstrcat(r->pool, ctx->header_path, ".XXXXXX", NULL);
    rv = apr_file_mktemp(&hf, tmp_header_path,
                          APR_FOPEN_CREATE | APR_FOPEN_READ | APR_FOPEN_WRITE | APR_FOPEN_EXCL,
                          r->pool);
    if (rv != APR_SUCCESS) {
        ap_log_rerror(APLOG_MARK, APLOG_ERR, rv, r,
                      "cacher: could not create temp header file for '%s'", ctx->header_path);
        apr_file_remove(ctx->tmp_body_path, r->pool);
        return;
    }

    if (apr_file_write_full(hf, &meta, sizeof(meta), NULL) != APR_SUCCESS
        || apr_file_write_full(hf, header_text, strlen(header_text), NULL) != APR_SUCCESS) {
        ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r,
                      "cacher: could not write temp header file for '%s'", ctx->header_path);
        apr_file_close(hf);
        apr_file_remove(tmp_header_path, r->pool);
        apr_file_remove(ctx->tmp_body_path, r->pool);
        return;
    }
    apr_file_close(hf);

    /* Body first, header last: a reader only trusts an entry once its
     * header file exists, so the header must never appear before the
     * body it describes. */
    if (apr_file_rename(ctx->tmp_body_path, ctx->body_path, r->pool) != APR_SUCCESS
        || apr_file_rename(tmp_header_path, ctx->header_path, r->pool) != APR_SUCCESS) {
        ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r,
                      "cacher: could not publish cache entry for '%s'", ctx->body_path);
        apr_file_remove(ctx->tmp_body_path, r->pool);
        apr_file_remove(tmp_header_path, r->pool);
        apr_file_remove(ctx->body_path, r->pool);
        return;
    }
}

static apr_status_t cacher_output_filter(ap_filter_t *f, apr_bucket_brigade *bb)
{
    request_rec *r = f->r;
    cacher_out_ctx *ctx = f->ctx;
    apr_bucket *e;
    if (!ctx->started) {
        ctx->started = 1;
        ctx->cacheable = cacher_rule_allows_status(ctx->rule, r->status)
                       && r->main == NULL
                       && !apr_table_get(r->headers_in, "Authorization");

        if (ctx->cacheable) {
            apr_status_t rv;
            char *shard_dir = apr_pstrndup(r->pool, ctx->body_path,
                                            strrchr(ctx->body_path, '/') - ctx->body_path);

            rv = apr_dir_make_recursive(shard_dir, APR_FPROT_OS_DEFAULT, ctx->pool);
            if (rv == APR_SUCCESS) {
                ctx->tmp_body_path = apr_pstrcat(ctx->pool, ctx->body_path, ".XXXXXX", NULL);
                rv = apr_file_mktemp(&ctx->tmp_body_file, ctx->tmp_body_path,
                                      APR_FOPEN_CREATE | APR_FOPEN_READ | APR_FOPEN_WRITE | APR_FOPEN_EXCL,
                                      ctx->pool);
            }
            if (rv != APR_SUCCESS) {
                ap_log_rerror(APLOG_MARK, APLOG_ERR, rv, r,
                              "cacher: could not open temp cache file under '%s'", shard_dir);
                ctx->cacheable = 0;
            }
        }

        if (!ctx->cacheable) {
            ap_remove_output_filter(f);
        }
    }

    for (e = APR_BRIGADE_FIRST(bb); e != APR_BRIGADE_SENTINEL(bb); e = APR_BUCKET_NEXT(e)) {
        if (ctx->cacheable && APR_BUCKET_IS_EOS(e)) {
            finalize_cache_entry(r, ctx);
            continue;
        }

        if (ctx->cacheable && !APR_BUCKET_IS_METADATA(e)) {
            const char *data;
            apr_size_t len;

            if (apr_bucket_read(e, &data, &len, APR_BLOCK_READ) == APR_SUCCESS && len > 0
                && apr_file_write_full(ctx->tmp_body_file, data, len, NULL) != APR_SUCCESS) {
                ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r,
                              "cacher: write failed for temp cache file '%s' - abandoning entry",
                              ctx->tmp_body_path);
                apr_file_close(ctx->tmp_body_file);
                apr_file_remove(ctx->tmp_body_path, r->pool);
                ctx->cacheable = 0;
                ap_remove_output_filter(f);
            }
        }
    }

    return ap_pass_brigade(f->next, bb);
}

void cacher_cache_insert_filter(request_rec *r, const char *cache_root, const cacher_rule *rule)
{
    cacher_out_ctx *ctx;
    char *header_path;
    char *body_path;

    if (!cache_root || rule->ttl_seconds <= 0) {
        return;
    }

    if (cache_key_paths(r, cache_root, rule, &header_path, &body_path) != 0) {
        return;
    }

    ctx = apr_pcalloc(r->pool, sizeof(*ctx));
    ctx->rule = rule;
    ctx->header_path = header_path;
    ctx->body_path = body_path;
    ctx->pool = r->pool;
    ctx->started = 0;

    ap_add_output_filter(CACHER_OUTPUT_FILTER_NAME, ctx, r, r->connection);
}

apr_file_t *cacher_cache_lookup(request_rec *r, const char *cache_root,
                                 const cacher_rule *rule,
                                 int *out_status, apr_off_t *out_length)
{
    char *header_path;
    char *body_path;
    apr_file_t *hf;
    apr_file_t *bf;
    cacher_cache_meta meta;
    apr_finfo_t finfo;
    char *header_text;
    apr_off_t header_text_len;
    char *line;
    char *last;

    if (!cache_root) {
        return NULL;
    }

    if (cache_key_paths(r, cache_root, rule, &header_path, &body_path) != 0) {
        return NULL;
    }

    if (apr_file_open(&hf, header_path, APR_FOPEN_READ, APR_FPROT_OS_DEFAULT, r->pool) != APR_SUCCESS) {
        return NULL; /* MISS */
    }

    if (apr_file_read_full(hf, &meta, sizeof(meta), NULL) != APR_SUCCESS || meta.magic != CACHER_CACHE_MAGIC) {
        apr_file_close(hf);
        return NULL;
    }

    if (meta.expires <= apr_time_now()) {
        apr_file_close(hf);
        return NULL; /* expired -> MISS */
    }

    if (apr_file_info_get(&finfo, APR_FINFO_SIZE, hf) != APR_SUCCESS) {
        apr_file_close(hf);
        return NULL;
    }
    header_text_len = finfo.size - (apr_off_t) sizeof(meta);
    if (header_text_len < 0) {
        apr_file_close(hf);
        return NULL;
    }

    header_text = apr_palloc(r->pool, (apr_size_t) header_text_len + 1);
    if (header_text_len > 0
        && apr_file_read_full(hf, header_text, (apr_size_t) header_text_len, NULL) != APR_SUCCESS) {
        apr_file_close(hf);
        return NULL;
    }
    header_text[header_text_len] = '\0';
    apr_file_close(hf);

    for (line = apr_strtok(header_text, "\r\n", &last); line; line = apr_strtok(NULL, "\r\n", &last)) {
        char *colon = strchr(line, ':');
        char *name;
        char *value;

        if (!colon) {
            continue;
        }
        *colon = '\0';
        name = line;
        value = colon + 1;
        while (*value == ' ') {
            value++;
        }
        if (cacher_header_name_is(name, "Content-Type")) {
            /* Apache generates the Content-Type response header from
             * r->content_type, not from headers_out - setting only the
             * table entry here would yield a wrong or missing type on
             * every cache hit. */
            ap_set_content_type(r, apr_pstrdup(r->pool, value));
        } else if (!is_uncacheable_header(name)) {
            apr_table_setn(r->headers_out, apr_pstrdup(r->pool, name), apr_pstrdup(r->pool, value));
        }
    }

    if (apr_file_open(&bf, body_path, APR_FOPEN_READ, APR_FPROT_OS_DEFAULT, r->pool) != APR_SUCCESS) {
        /* Header published but body missing - a concurrent write was
         * interrupted mid-rename. Treat as MISS rather than serving a
         * headers-only response. */
        return NULL;
    }

    if (apr_file_info_get(&finfo, APR_FINFO_SIZE, bf) != APR_SUCCESS) {
        apr_file_close(bf);
        return NULL;
    }

    *out_status = meta.status;
    *out_length = finfo.size;
    return bf;
}
