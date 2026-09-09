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

### Verified so far

- Compiles clean (`-Wall -Wextra`) on Ubuntu 24.04 against Apache 2.4.x
- Unit tests pass (`make test`)
- Loads into a live Apache and survives a restart
- Directives parse; `apachectl configtest` returns `Syntax OK`
- Inert when no directory sets `CacherEnable On` - existing sites unaffected

- **End-to-end cache hit driven purely by `.htaccess`** - a second request
  is served from disk without the backend re-running, with only
  `CacherCacheRoot` set at server level and all rules coming from
  `CacherEnable` + `CacherRulesFile` in a directory's `.htaccess`
- Sharded on-disk layout written as designed
  (`<root>/9e/1a/9e1af0c7….{body,header}`)

### Not yet verified

- Cookie bypass, TTL expiry, `vary` partitioning, `methods` exclusion,
  behaviour under concurrent regeneration.

### Note on log locations

`ap_log_rerror` messages go to the **vhost's** `ErrorLog`, not the main
server log - check the vhost's own file when looking for `cacher:` trace
lines.

## Build (Linux, primary target)

Requires the Apache dev headers (`apache2-dev` / `httpd-devel`) for `apxs`.

```sh
make            # compile only - touches nothing outside this directory
make install    # apxs -i: copies the .so into Apache's modules dir only
make enable     # apxs -i -a: also adds LoadModule (edits Apache config)
make test       # standalone rule-parser unit tests, no Apache needed
make warn       # rebuild with -Wall -Wextra, showing only our own code
```

Verify the module loaded: `apachectl -M | grep cacher`.

## Deploying to a staging server

Do this on staging only, on a throwaway vhost - not in front of anything
that matters, and not over an authenticated area on the first pass.

### Will this disturb an Apache that's already running?

Steps 1-5 below will not. Building (`make`) and installing (`make install`)
only write files - they never touch Apache's config and never load
anything. A running server carries on untouched. You can get the single
most valuable signal here (does it compile at all?) at zero risk.

The risk begins only when you add `LoadModule` and restart. From then on:

- **The module runs on every request server-wide**, because it registers a
  content handler at `APR_HOOK_FIRST`. It exits immediately with `DECLINED`
  unless that directory has `CacherEnable On` (the default is Off), so the
  active code
  path is a config lookup and one integer comparison - but it *is* running
  everywhere, so a bug in that path affects the whole server, not just
  cached directories.
- **A crash takes out the Apache worker handling that request.** With
  prefork/worker MPMs, Apache respawns the child, so it usually shows up
  as intermittent 500s or dropped connections rather than a hard outage.
- **A failure at startup keeps Apache from starting at all** - which is
  why `apachectl configtest` before every restart is non-negotiable.

### Getting back to a working Apache

Everything this module installs is additive and removable. It puts exactly
four things on a server:

1. `mod_cacher.so` in Apache's modules directory (a new file - no existing
   module is named this, so nothing is overwritten)
2. one `LoadModule` line in the Apache config
3. the `CacherCacheRoot` directory, and cache files beneath it
4. whatever `.htaccess` / rules JSON you add yourself

**It never writes to or deletes anything outside `CacherCacheRoot`.** Every
cache path is built as `<root>/<2 hex>/<2 hex>/<32 hex>.{header,body}` from
an MD5 hex digest - `[0-9a-f]` only - so no request can direct a write or a
delete outside that tree.

Undo it completely (RHEL/Amazon Linux paths; Debian/Ubuntu uses
`/etc/apache2/apache2.conf` and `/usr/lib/apache2/modules`):

```bash
sudo cp /etc/httpd/conf/httpd.conf.bak /etc/httpd/conf/httpd.conf   # or: sudo sed -i '/mod_cacher/d' /etc/httpd/conf/httpd.conf
sudo apachectl configtest && sudo systemctl restart httpd
sudo rm -f /etc/httpd/modules/mod_cacher.so
sudo rm -rf /var/cache/cacher
```

Apache is then in exactly the state it was before. If it won't start at
all, the first two lines are the whole fix - the `.so` and cache files are
inert once nothing loads them.

**Safest option of all** - don't load it into the live Apache at first.
Run a throwaway instance on another port with its own minimal config:

```bash
sudo httpd -f /opt/cacher/test.conf -X -e debug
```

A minimal `test.conf` needs `Listen 8080`, the `LoadModule` lines for the
MPM/core modules your build requires plus `mod_cacher`, a `DocumentRoot`,
`CacherCacheRoot`, and an `ErrorLog`. `-X` keeps it in the foreground as a
single process, so a segfault ends that process and nothing else - your
production Apache on :80 never notices.

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

`make install` runs `apxs -i`: it copies the `.so` into Apache's modules
directory and **does nothing else**. It does not edit any config and does
not load the module, so a running Apache is completely unaffected at this
point. Nothing changes until you add the `LoadModule` line yourself.

Back up the config before that:

```bash
sudo cp /etc/httpd/conf/httpd.conf /etc/httpd/conf/httpd.conf.bak   # RHEL/Amazon Linux
```

Then add the line (adjust the path to match the other `LoadModule` lines
on your system):

```apache
LoadModule cacher_module modules/mod_cacher.so
```

**Always validate before restarting** - this is what stops a bad module
from taking the site down:

```bash
sudo apachectl configtest && sudo apachectl -k graceful
```

If `configtest` fails, do not restart: the currently running Apache keeps
serving with its old config, and you can just remove the line. Confirm the
module loaded with `apachectl -M | grep cacher`.

(`make enable` does the install *and* the config edit in one step via
`apxs -i -a`. It's there for convenience but skips the backup and the
configtest gate, so prefer the manual route on any server that matters.)

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
  restart, and capture the error log. A crash in the handler takes
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

- **Cache hits run after authentication, not before.** The read path is a
  content handler rather than a `quick_handler`, so a hit cannot be served
  to a request that would have failed auth. This is a consequence of
  needing `.htaccess` config at all: `quick_handler` fires before
  `directory_walk`, so per-directory rules simply do not exist yet there -
  which is why `mod_cache`'s own `CacheEnable` is server-config-only. Hits
  still skip the content generator (PHP and friends), which is where the
  cost of a dynamic request actually lives.
- **Origin `Cache-Control` is ignored.** Cacher obeys only its own JSON
  rules, so a backend replying `Cache-Control: no-store` will still be
  cached if a rule matches. WordPress sends exactly that on normal page
  responses, so point rules at paths you have actually reasoned about.
- **`Set-Cookie` is never cached or replayed**, even for otherwise
  cacheable responses - it's stripped on both the write and read path so
  one visitor's session-establishing cookie can never leak to another
  visitor served the same cached entry. The rest of the response is still
  cached as normal.
- Disk cache is local-filesystem only (no NFS/shared-storage support),
  single-process-safe via atomic rename, not cross-process locked.
- Windows: `MAX_PATH` on sharded cache paths, and `apr_file_rename`
  overwrite semantics differing from POSIX `rename()`, are untested.
