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

/* Case-insensitive exact match. Used for HTTP header field names (whose
 * case Apache does not normalise) and for hostnames. */
int cacher_streq_ci(const char *a, const char *b);

/*
 * Returns the outermost request in an internal-redirect chain - the one
 * holding the URI the client actually asked for.
 *
 * Front-controller rewrites do an internal redirect rather than a plain
 * URI edit. WordPress's "RewriteRule . /index.php [L]" is the common case:
 * by the time a handler runs, r->uri is "/index.php" for every page on the
 * site. Matching rules against that defeats every path-based exclusion
 * (the exclusion fires on the original URI, then the redirected request
 * sails past it), and keying the cache on it makes every page on the site
 * collide into a single entry.
 */
request_rec *cacher_original_request(request_rec *r);

#endif /* CACHER_UTIL_H */
