# Troubleshooting

Every problem below was hit while actually installing this module on an
Ubuntu 24.04 EC2 host running WordPress + WooCommerce. Symptom first, then
cause, then fix.

A recurring theme: **most failures here are silent**. The module declines and
gets out of the way rather than erroring, so "nothing happens" is the usual
symptom. When something does not work, the vhost error log with
`LogLevel info cacher:trace1` is nearly always the fastest answer.

---

## Installation

### apt is locked

```
E: Could not get lock /var/lib/apt/lists/lock. It is held by process 59089 (apt-get)
```

Find out what holds it, and crucially **how long it has held it**:

```bash
ps -o pid,ppid,etime,cmd -p 59089
```

**If `ELAPSED` is seconds or minutes**, it is normal activity - wait:

```bash
while kill -0 59089 2>/dev/null; do echo waiting; sleep 5; done; echo free
```

**If it has been running for hours or days, it is wedged.** What you may do
next depends entirely on which command it is:

| Process | Safe to kill? |
|---|---|
| `apt-get update` | **Yes.** It only fetches index files and runs no dpkg transaction, so nothing can be left half-configured. |
| `unattended-upgrades`, `apt-get install/upgrade`, `dpkg` | **No.** These run dpkg transactions; killing one can leave packages half-configured. Wait it out. |

To kill a wedged `apt-get update`:

```bash
kill 59089 && sleep 5 && kill -0 59089 2>/dev/null && kill -9 59089
rm -rf /var/lib/apt/lists/partial/*
dpkg --audit          # should print nothing
apt-get update
```

Never delete lock files - that fixes nothing and can break dpkg's state.

### apt is locked again, by unattended-upgrades

Freeing a long-stuck lock often lets a backlog of security updates finally
run. That is expected and healthy. **Wait for it** - it runs dpkg. Watch
progress:

```bash
tail -f /var/log/unattended-upgrades/unattended-upgrades.log
```

Do **not** wait with a loop like `while pgrep -x unattended-upgr; do ...`.
A permanent daemon called `unattended-upgrade-shutdown` matches that name
and runs for the life of the boot, so the loop never exits. Wait on the
specific PID instead, or just retry the install and read the error.

### `git clone` warns the GitHub host key changed

```
WARNING: REMOTE HOST IDENTIFICATION HAS CHANGED!
```

Usually a stale entry: GitHub rotated its RSA host key in March 2023 after
briefly exposing the private key, so `known_hosts` entries older than that
no longer match.

**Verify before trusting it.** Compare the fingerprint in the warning to
GitHub's published one:

```bash
curl -s https://api.github.com/meta | python3 -c "import sys,json;print(json.load(sys.stdin)['ssh_key_fingerprints'])"
```

If it matches, remove the stale entry and re-add the current keys over
HTTPS rather than trusting whatever answers on port 22:

```bash
ssh-keygen -R github.com
curl -s https://api.github.com/meta | python3 -c "import sys,json;[print('github.com',k) for k in json.load(sys.stdin)['ssh_keys']]" >> ~/.ssh/known_hosts
ssh -T git@github.com
```

If it does **not** match, stop and investigate.

### Cloning a private repo

```
git@github.com: Permission denied (publickey).
```

The server has no key for the repository. Either use HTTPS if the repo is
public, or add a read-only deploy key:

```bash
ssh-keygen -t ed25519 -f /root/.ssh/gh_deploy -N "" -C "$(hostname)"
cat /root/.ssh/gh_deploy.pub
```

Add that public key under the repo's **Settings → Deploy keys**, leaving
write access unchecked. Then:

```bash
printf 'Host github.com\n  IdentityFile /root/.ssh/gh_deploy\n  IdentitiesOnly yes\n' >> /root/.ssh/config
ssh -T git@github.com
```

Use `GIT_TERMINAL_PROMPT=0 git clone …` when testing, so a private repo
fails fast instead of hanging on a credential prompt.

### `git push` suddenly fails on your own machine

```
git@github.com: Permission denied (publickey).
```

Typically a passphrase-protected key with no `ssh-agent` running - common
after a reboot. Check:

```bash
ssh-keygen -y -P "" -f ~/.ssh/id_github >/dev/null 2>&1 && echo "no passphrase" || echo "passphrase-protected"
```

Start an agent and load the key, then push **from that same shell** - the
agent only exists there:

```bash
eval $(ssh-agent -s) && ssh-add ~/.ssh/id_github
```

### `make install` fails with "cannot access mod_cacher.so"

```
libtool: install: install src/mod_cacher.c /usr/lib/apache2/modules/mod_cacher.c
chmod: cannot access '/usr/lib/apache2/modules/mod_cacher.so': No such file or directory
```

Fixed in current versions - `apxs -i` must be given the built `.la` archive,
not the source list. If you are on an older checkout, `git pull`. To
recover by hand, and clean up the stray `.c` it copied:

```bash
cp /opt/cacher/src/.libs/mod_cacher.so /usr/lib/apache2/modules/mod_cacher.so
chmod 644 /usr/lib/apache2/modules/mod_cacher.so
rm -f /usr/lib/apache2/modules/mod_cacher.c
```

### `cd: /d/server/...: No such file or directory`

You ran a Windows path on the Linux server. The repo lives in two places -
your workstation and `/opt/cacher` on the server. `git push` runs on your
workstation; `git pull` runs on the server.

---

## Caching does not happen

Work down this list; each step is cheap.

### 0. Read the `X-Cacher` header first

It names the outcome and usually ends the investigation immediately:

```bash
curl -sI https://your-site.example/some-page/ | grep -i '^x-cacher\|^age'
```

`HIT` cached · `MISS` stored just now · `BYPASS` a cookie matched
`bypass_cookies` · `EXCLUDED` an exclusion rule matched · `DYNAMIC` no rule
matched · **no header at all** means the module never engaged - `CacherEnable`
is not in effect, so go to step 2.

**Do not judge this from a browser you are logged into.** Your session
cookies will produce `BYPASS` on every request, which is the module working
correctly and looks identical to it being broken. Use `curl`, or a private
window.

Another quick tell without any header: a cache hit sets `Content-Length`,
whereas a freshly generated PHP page is usually `Transfer-Encoding: chunked`.

### 1. Is the module even loaded?

```bash
apache2ctl -M | grep cacher
```

### 2. Is `CacherEnable On` actually taking effect?

The commonest cause of total silence. Look at the file:

```bash
tail -5 /path/to/docroot/.htaccess
```

**Watch for this:**

```
# END WordPressCacherEnable On
```

WordPress writes `.htaccess` without a trailing newline, so appending with
`cat >>` or `echo >>` glues your first directive onto the end of the last
line - where `#` comments it out. No error, no log, nothing cached. Fix:

```bash
sed -i 's/^# END WordPressCacherEnable On$/# END WordPress\nCacherEnable On/' /path/to/docroot/.htaccess
```

And always append with a leading newline:

```bash
printf '\nCacherEnable On\n' >> .htaccess
```

### 3. Are `.htaccess` overrides allowed?

With `AllowOverride None` (Ubuntu's default for `/var/www/`), Apache does
not read `.htaccess` at all - silently.

```bash
grep -r -i allowoverride /etc/apache2/sites-enabled/ /etc/apache2/apache2.conf
```

You need `All` or `FileInfo` for the site's directory.

### 4. Are you reading the right log?

Request-level messages go to the **vhost's** `ErrorLog`, not the main
server log:

```bash
grep -i errorlog /etc/apache2/sites-enabled/<your-site>*.conf
grep cacher /var/log/apache2/<your-site>-error.log | tail -20
```

With `LogLevel info cacher:trace1` you should see one line per decision:
`served from cache`, `matched no rule`, `matched an exclusion rule`,
`bypassed (matching cookie present)`, or `no usable rules`.

### 4b. Does the rules file still exist?

```bash
ls -l /path/to/cacher-rules.json
```

If the rules file lives inside the document root and the site is deployed
from git, a deploy that removes untracked files will delete it - after
which the module declines every request, logging only
`enabled here but no usable rules` at `trace1`.

Keep it outside the deployable tree and point at it absolutely:

```apache
CacherRulesFile /etc/cacher/example.com.json
```

### 5. Is the rules file parsing?

A parse failure logs once per request:

```
cacher: failed to parse CacherRules: JSON syntax error near: ...
```

```bash
python3 -m json.tool /path/to/docroot/cacher-rules.json >/dev/null && echo "valid JSON"
```

When using inline `CacherRules`, wrap the JSON in **single quotes** so its
own double quotes survive.

### 6. Is a bypass cookie always present?

`curl` sends no cookies, but a browser does. If `bypass_cookies` lists a
cookie your site sets for *everyone* - `PHPSESSID` is the classic case -
then every real visitor bypasses and the cache never serves anyone. Use
cookies that only logged-in or transacting users have:
`wordpress_logged_in_*`, `wp_woocommerce_session_*`, and similar.

Check what your site sets for an anonymous visitor:

```bash
curl -s -D - -o /dev/null https://your-site.example/ | grep -i set-cookie
```

---

## Caching happens, but wrongly

### Every page returns the same content

Front-controller rewrites (`RewriteRule . /index.php [L]`) internally
redirect, so without care every URL looks like `/index.php` and the whole
site collapses into one cache entry.

Fixed in current versions - both rule matching and the cache key use the
URL the client actually requested. If you see it, `git pull && make && make
install && systemctl restart apache2`, then `cacher full-reset`.

Verify with content, not entry counts:

```bash
curl -s https://your-site.example/ > /tmp/a; curl -s https://your-site.example/some-page/ > /tmp/b; diff -q /tmp/a /tmp/b
```

Those must differ.

### Exclusions are ignored

Rules are ordered and the **first match wins, including exclusions**. Every
`"enabled": false` rule must appear *above* the catch-all. A rule placed
after it is unreachable.

### The cache fills with CSS, JS and images

The catch-all matches everything unless constrained. Give it
`"content_types": ["text/html"]` and exclude asset directories by path -
see [RULES.md](RULES.md). Apache already serves static files well; caching
them costs disk and adds a lookup to the majority of your requests.

### The cache grows without bound

The query string is part of the cache key, so every distinct
`?utm_source=…` becomes its own entry. Exclude query strings unless you
need them:

```json
{ "match": { "query": "?*" }, "enabled": false }
```

There is no eviction beyond TTL overwrite. A stopgap reaper:

```bash
find /var/cache/cacher -type f -mmin +60 -delete
```

### Edited content still shows the old page

Expected: nothing invalidates the cache on content change, it just expires.
Purge explicitly:

```bash
cacher purge '/about/'
```

Or lower the rule's `ttl`.

---

## Admin endpoints

### `/cacher-admin/list` returns the site's 404 page

Two possible causes, in order of likelihood:

**`CacherAdminPath` was never set.** It is off by default:

```bash
grep -r CacherAdminPath /etc/apache2/ /path/to/docroot/.htaccess
```

**The app's rewrite swallowed it.** WordPress routes unmatched URLs to
`index.php`. Exempt the path, above the app's rules:

```apache
<IfModule mod_rewrite.c>
RewriteEngine On
RewriteRule ^cacher-admin - [END]
</IfModule>
```

The module deliberately refuses a rewritten admin request rather than
serving it, because `<Location>` matches the post-rewrite URI - so any
`Require` guarding the path would no longer apply. It logs the exact
`RewriteRule` to add.

### `/cacher-admin/…` returns 403

No authenticated user, and `CacherAdminRequireUser` defaults to `On`. Either
add auth, or accept unauthenticated access explicitly:

```apache
CacherAdminRequireUser Off
```

An IP allowlist (`Require ip`) leaves no authenticated user, so those setups
need `Off` too - Apache still enforces the allowlist.

### `list` shows nothing, but the CLI shows entries

Expected. The HTTP endpoints only ever act on entries for the host the
request came in on; the CLI is server-wide. Narrow the CLI to compare:

```bash
cacher list --host your-site.example
```

---

## Quick reference

```bash
apache2ctl -M | grep cacher                        # module loaded?
apache2ctl configtest                              # config valid?
tail -5 /path/to/docroot/.htaccess                 # directives on their own lines?
python3 -m json.tool /path/to/cacher-rules.json    # rules valid JSON?
grep cacher /var/log/apache2/<site>-error.log      # per-request decisions
cacher list                                        # what is actually cached
cacher full-reset                                  # start clean
a2dismod cacher && systemctl restart apache2       # turn it all off
```
