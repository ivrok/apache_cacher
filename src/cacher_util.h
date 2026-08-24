#ifndef CACHER_UTIL_H
#define CACHER_UTIL_H

#include "httpd.h"

#include "cacher_rules.h"

/*
 * True if any cookie on this request has a name matching one of `rule`'s
 * bypass_cookies glob patterns. Used to skip caching - both read and write
 * - for authenticated/personalized requests.
 */
int cacher_request_bypasses(request_rec *r, const cacher_rule *rule);

/*
 * Looks up a single cookie's value by exact name (cookie names are
 * case-sensitive per RFC 6265). Returns NULL if not present. Used for
 * `vary: ["cookie:NAME"]` cache-key partitioning.
 */
const char *cacher_get_cookie(request_rec *r, const char *name);

/* Case-insensitive exact match, for comparing HTTP header field names
 * (whose case Apache does not normalize). */
int cacher_header_name_is(const char *name, const char *target);

#endif /* CACHER_UTIL_H */
