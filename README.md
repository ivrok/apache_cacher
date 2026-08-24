# Cacher

An Apache httpd module for JSON-configurable, cookie-aware page caching -
set caching rules per directory via `.htaccess`, similar in spirit to
Cloudflare Cache Rules, including bypassing the cache when a visitor's
session/auth cookie is present.

## Status

In development. See the phase checklist below; each phase is independently
buildable and curl-testable.

- [x] Phase 1 - module skeleton (loads, directives parse, hooks wired)
- [x] Phase 2 - JSON rule parsing + per-directory rules resolution/caching
- [x] Phase 3 - disk cache write path (cache MISS)
- [x] Phase 4 - cache read path + cookie bypass (cache HIT)
- [x] Phase 5 - purge/inspection tooling (filesystem-based, documented below - no code needed, by design)
- [ ] Phase 6 - Windows/WAMP portability pass

Phases 3 and 4 were implemented together rather than strictly in sequence:
the write path can only be correct once cookie-bypass is in place (an
authenticated request must never be captured into the shared cache), so
splitting them would have meant a real, if temporary, security gap.

**None of this has been compiled or run yet** - see "Status" note in the
project history: no C toolchain was available in the environment this was
written in. Treat it as a careful-first-draft, not as verified working
code, until it's built and exercised with the curl matrix below.

## Build (Linux, primary target)

Requires the Apache dev headers (`apache2-dev` / `httpd-devel`) for `apxs`.

```sh
make            # compile only
make install    # apxs -i -a: installs the module and adds LoadModule
make test       # builds and runs the standalone rule-parser unit tests
```

Verify the module loaded: `httpd -M | grep cacher`.

## Deploying to a staging server

Do this on staging only, on a throwaway vhost - not in front of anything
that matters, and not over an authenticated area on the first pass.

**1. Copy the project over**

```bash
rsync -av --exclude='.git' ./Cacher/ user@staging:/opt/cacher/
```

**2. Install the Apache module build tooling**

```bash
sudo apt-get install -y apache2-dev build-essential   # Debian/Ubuntu
```

(RHEL/Alma/Rocky: `sudo dnf install -y httpd-devel gcc make`.)

**3. Run the unit tests first - they need no Apache at all**

```bash
cd /opt/cacher && make test
```

This exercises the JSON rule parser and glob matcher standalone. If this
fails, fix it before touching Apache.

**4. Build the module**

```bash
cd /opt/cacher && make
```

This is the first real compile of this code - expect to fix errors here.

**5. Install it**

```bash
cd /opt/cacher && sudo make install
```

`apxs -i -a` drops the `.so` in the modules dir and adds the `LoadModule`
line. Confirm: `apachectl -M | grep cacher`.

**6. Create the cache directory, owned by the Apache runtime user**

```bash
sudo install -d -o www-data -g www-data /var/cache/cacher
```

(`apache:apache` on RHEL-family.) The module creates this at startup too,
but doing it explicitly gets the ownership right the first time.

**7. Point the server at it** - in the staging vhost or server config:

```apache
CacherCacheRoot /var/cache/cacher
```

`CacherCacheRoot` is server/vhost-level only and is required - without it
Cacher parses rules but never caches anything.

**8. Make sure `.htaccess` overrides are allowed** for the test directory:

```apache
<Directory /var/www/staging/public>
    AllowOverride FileInfo
</Directory>
```

(`AllowOverride All` also works.) Without `FileInfo`, the `Cacher*`
directives in `.htaccess` will produce a 500 and an "not allowed here"
error in the log.

**9. Turn on module logging so you can see HIT/MISS/bypass decisions**

```apache
LogLevel info cacher:trace1
```

**10. Drop in a rules file** - copy `examples/htaccess.example` to
`.htaccess` and `examples/cacher-rules.example.json` to
`cacher-rules.json` in the test directory, adjusting the paths and cookie
names to match the app. Start with a short `"ttl"` (say 30) so mistakes
expire quickly.

**11. Restart and run the curl matrix below**

```bash
sudo apachectl configtest && sudo apachectl -k graceful
```

Note that steps 1-5 need repeating on every code change, but editing
`.htaccess` or the rules JSON needs **no restart** - that's the property
worth confirming early (curl matrix item 6).

### If something goes wrong

- Segfault / Apache won't start → comment out the `LoadModule` line,
  restart, and capture the error log. A crash in a `quick_handler` takes
  down the worker, which is exactly why this belongs on staging first.
- 500s on every request → almost always `AllowOverride` (step 8) or a
  JSON syntax error; check the error log, the module logs parse failures
  with the offending text.
- Nothing ever caches → check `CacherCacheRoot` is set (step 7) and
  writable by the Apache user (step 6), and that `ttl` is > 0.

## Windows / WAMP

Not yet built/tested here - no C toolchain was available in the dev
environment this was written in. To build with MSVC against a WAMP64
Apache install:

```
cl /LD /MD /I"<apache>\include" /Ithird_party ^
   src\mod_cacher.c src\cacher_config.c src\cacher_rules.c ^
   src\cacher_cache.c src\cacher_util.c third_party\cJSON.c ^
   /link /LIBPATH:"<apache>\lib" libhttpd.lib libapr-1.lib libaprutil-1.lib /OUT:mod_cacher.so
```

See the open risks section below (esp. `MAX_PATH` and `apr_file_rename`
semantics) before relying on this in production on Windows.

## Directives

| Directive | Context | Description |
|---|---|---|
| `CacherEnable On\|Off` | `.htaccess`, `<Directory>` | Enable Cacher for this directory. **Default: Off** - must be explicitly opted in. |
| `CacherRules '<json>'` | `.htaccess`, `<Directory>` | Inline JSON rules (see schema below). Re-parsed every request - fine for small rule sets. |
| `CacherRulesFile <path>` | `.htaccess`, `<Directory>` | Path to a JSON rules file, relative to the directory it's set in. Wins over `CacherRules` if both are set. Cached in memory, keyed by file mtime/size - only re-read when the file actually changes. |
| `CacherCacheRoot <path>` | server/vhost config only | Filesystem root for cached responses. Created automatically at startup (`post_config`) if missing. Required for `CacherEnable On` to actually cache anything. |

## JSON rules schema

See [`examples/cacher-rules.example.json`](examples/cacher-rules.example.json).

```json
{
  "rules": [
    {
      "match": { "path": "/blog/*", "methods": ["GET", "HEAD"] },
      "enabled": true,
      "ttl": 300,
      "status_codes": [200],
      "bypass_cookies": ["PHPSESSID", "wordpress_logged_in_*"],
      "vary": ["Accept-Encoding", "cookie:cart_id"]
    }
  ]
}
```

- `rules` is ordered; the **first matching rule wins**. No match ⇒ do not cache.
- `match.path` - glob pattern (`*`/`?`), matched against the request path. Omit to match any path.
- `match.methods` - array of HTTP methods. Omit to match any method.
- `bypass_cookies` - glob patterns matched against cookie **names** (never values) present on the request. Any match skips the cache entirely for that request - this is how an authenticated session avoids being served/stored as a cached page.
- `status_codes` - response statuses eligible for caching. Defaults to `[200]`.
- `vary` - header names, plus an optional `cookie:<name>` pseudo-entry to partition the cache by a cookie's value.

## Verifying end-to-end (curl matrix)

Once built and loaded, with `CacherEnable On` + a rule matching some path:

1. `curl -i http://host/path` twice - first response generated by the
   backend (MISS, `.header`/`.body` files appear under `CacherCacheRoot`);
   second response served from cache (backend does not re-run - check via
   a timestamp in the page, or `LogLevel trace1` module logs).
2. `curl -i -b "PHPSESSID=x" http://host/path` - always passes through to
   the backend, even though a cookie-less cached entry exists.
3. Use a short `"ttl"` (e.g. `2`) - confirm a MISS again after it expires.
4. Two requests differing only in a `vary`-listed header/cookie - confirm
   two distinct files appear under `CacherCacheRoot`.
5. `curl -i -X POST http://host/path`, or a route returning a status not
   in `status_codes` - confirm nothing is written to disk.
6. A nested `.htaccess` overriding a parent directory's rule - confirm the
   child's rule wins (standard Apache per-directory merge scoping).
7. `ab -c 20 -n 200 http://host/path` against a MISS URL - confirm no
   corrupted/partial cache files and no crash under concurrent regeneration.

## Purging the cache

There's no purge API in v1 - the on-disk layout is intentionally simple
enough that the filesystem itself is the purge tool:

- **Purge everything**: stop relying on stale files being served (they
  expire on their own via `ttl`), or just `rm -rf` the contents of
  `CacherCacheRoot` - a fresh MISS regenerates each entry on next request.
- **Purge one entry**: it isn't practical to reverse-compute a specific
  URL's hash by hand; instead, either lower that rule's `ttl` temporarily,
  or clear the whole shard prefix it happens to fall under (the first 4
  hex characters of the MD5 of `METHOD\nHost\nPath?query\n...vary...`).
- Because writes are atomic (temp file + rename), it's always safe to
  delete cache files while the server is running - a request mid-flight
  either sees the old file or a fresh MISS, never a corrupt read.

## Project layout

```
src/             module source (mod_cacher.c + cacher_{config,rules,cache,util}.{h,c})
third_party/     vendored cJSON (MIT) - see third_party/cJSON.h for license
test/            standalone unit tests (no Apache headers required)
examples/        sample .htaccess + rules file
```

## Open risks

See the plan history for the full list; the ones most likely to bite:

- **Auth-bypass blast radius**: a HIT is served before Apache's auth phases
  run. A rule enabled under an authenticated directory whose
  `bypass_cookies` doesn't cover the app's real session cookie name could
  leak private content to anonymous users. `CacherEnable` defaults to
  **Off** for this reason - enable it deliberately, per directory.
- **`Set-Cookie` is never cached or replayed**, even for otherwise
  cacheable responses - it's stripped on both the write and read path so
  one visitor's session-establishing cookie can never leak to another
  visitor served the same cached entry. The rest of the response is still
  cached as normal.
- Disk cache is local-filesystem only (no NFS/shared-storage support),
  single-process-safe via atomic rename, not cross-process locked.
- Windows: `MAX_PATH` on sharded cache paths, and `apr_file_rename`
  overwrite semantics differing from POSIX `rename()`, are untested.
