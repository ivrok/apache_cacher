#ifndef CACHER_CACHE_H
#define CACHER_CACHE_H

#include "httpd.h"

#include "cacher_rules.h"

/* Name under which the write-path output filter is registered. */
#define CACHER_OUTPUT_FILTER_NAME "CACHER_OUT"

/*
 * Response header naming the cache outcome, so "was this cached?" is
 * answerable from the client rather than only from the server's log:
 *
 *   HIT       served from the cache
 *   MISS      generated now, and stored for next time
 *   BYPASS    a bypass_cookies match - never cached for this visitor
 *   EXCLUDED  matched a rule with "enabled": false
 *   DYNAMIC   Cacher is on here, but no rule matched this request
 *
 * Suppress with "CacherStatusHeader Off".
 */
#define CACHER_STATUS_HEADER "X-Cacher"

/* Metadata recorded in a .header file, as parsed by cacher_cache_read_meta. */
typedef struct {
    apr_int64_t created;   /* unix seconds; 0 if written by an older version */
    apr_int64_t expires;   /* unix seconds */
    int status;
    const char *method;
    const char *host;
    const char *url;       /* path, plus ?query when present */
} cacher_entry_meta;

/*
 * Parses the plain-text metadata block of a .header file. Returns 0 on
 * success, non-zero if the file is missing, unreadable, or written in a
 * different on-disk format version.
 *
 * On success, *headers_out points at the response header block within the
 * same pool-allocated buffer (pass NULL if the headers aren't wanted).
 */
int cacher_cache_read_meta(apr_pool_t *p, const char *header_path,
                            cacher_entry_meta *meta, char **headers_out);

/* Registers the CACHER_OUT output filter type. Call once, from register_hooks. */
void cacher_cache_register_filter(apr_pool_t *p);

/*
 * Ensures every server_rec's configured CacherCacheRoot exists on disk.
 * Call once from post_config. Returns 0 on success, -1 if any configured
 * root could not be created (logged via ap_log_error).
 */
int cacher_cache_ensure_root(apr_pool_t *p, server_rec *base_s);

/*
 * Looks up a fresh cache entry for this request under `rule`. On a HIT,
 * parses the stored headers directly into r->headers_out (stripping
 * hop-by-hop headers that don't survive a cache replay), fills
 * *out_status and *out_length, and returns an open apr_file_t positioned
 * at the start of the cached body - the caller streams it out and closes
 * it. Returns NULL on MISS, expiry, or when cache_root is unset.
 *
 * *out_age receives the entry's age in seconds, or -1 if the entry predates
 * the recording of a creation time.
 */
apr_file_t *cacher_cache_lookup(request_rec *r, const char *cache_root,
                                 const cacher_rule *rule,
                                 int *out_status, apr_off_t *out_length,
                                 apr_int64_t *out_age);

/*
 * Inserts the CACHER_OUT filter that will capture this response to disk
 * if it turns out to be cacheable per `rule` (right status code, not a
 * subrequest, no Authorization header). Called from quick_handler on a
 * MISS, before DECLINEing. No-op if cache_root is unset or rule->ttl_seconds <= 0.
 */
void cacher_cache_insert_filter(request_rec *r, const char *cache_root, const cacher_rule *rule);

#endif /* CACHER_CACHE_H */
