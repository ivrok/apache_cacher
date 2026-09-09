#ifndef CACHER_ADMIN_H
#define CACHER_ADMIN_H

#include "httpd.h"

/*
 * HTTP endpoints for inspecting and purging the cache.
 *
 * Disabled unless CacherAdminPath is set, and that directive is
 * server-config only: allowing it in .htaccess would let anyone who can
 * write a file into the document root (a compromised CMS, for instance)
 * expose cache control on a public URL.
 *
 * These endpoints carry no authentication of their own, by design. The
 * handler runs after Apache's authentication and authorisation phases, so
 * wrapping the path in a <Location> with `Require ip ...` or Basic auth
 * protects it with the server's own, well-tested machinery rather than
 * something improvised here. An unprotected full-reset endpoint is a
 * denial-of-service button: every call forces the whole site to be
 * regenerated from the backend.
 *
 * `action` is the remainder of the URI after the configured admin path:
 *   ""            or "/"            -> usage text
 *   "/list"                          -> every cached entry
 *   "/purge/<glob>"                  -> delete entries whose URL matches
 *   "/full-reset"                    -> delete everything
 *
 * Returns an HTTP status suitable for returning from a content handler.
 */
int cacher_admin_handle(request_rec *r, const char *cache_root, const char *action);

#endif /* CACHER_ADMIN_H */
