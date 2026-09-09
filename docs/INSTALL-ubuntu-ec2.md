# Installing Cacher on an Ubuntu EC2 server

Written against Ubuntu 24.04 (noble) with Apache 2.4 on EC2. Commands assume
you are `root` (via SSM Session Manager or `sudo -i`); prefix with `sudo` if
not.

If a step fails, check [TROUBLESHOOTING.md](TROUBLESHOOTING.md) - every
problem listed there was actually hit while installing this.

---

## Before you start

Steps 1 to 5 **cannot disturb a running Apache**. They only compile code and
copy a file. Nothing changes for your sites until step 7 adds `LoadModule`
and restarts.

Rollback at any point after that is:

```bash
a2dismod cacher; a2disconf cacher; apache2ctl configtest && systemctl restart apache2
```

---

## 1. Install the build tooling

```bash
apt-get update && apt-get install -y apache2-dev build-essential git
```

If this fails with a lock error, see
[TROUBLESHOOTING.md § apt is locked](TROUBLESHOOTING.md#apt-is-locked).

Confirm the Apache extension tool is present - on some systems it is named
`apxs2`:

```bash
which apxs apxs2
```

If only `apxs2` exists, add `APXS=apxs2` to every `make` command below.

## 2. Get the source

```bash
git clone https://github.com/ivrok/apache_cacher.git /opt/cacher
```

For a private repository use SSH with a read-only deploy key - see
[TROUBLESHOOTING.md § cloning a private repo](TROUBLESHOOTING.md#cloning-a-private-repo).

## 3. Run the unit tests

They need no Apache at all, so run them before touching anything:

```bash
cd /opt/cacher && make test
```

Expect `All tests passed.` If not, stop here.

## 4. Compile

```bash
cd /opt/cacher && make
```

Still nothing outside `/opt/cacher` has been touched.

## 5. Install the module and CLI

```bash
cd /opt/cacher && make install
```

This copies `mod_cacher.so` into Apache's modules directory and the `cacher`
command into `/usr/local/bin`. It does **not** edit any Apache config and
does not load the module.

```bash
ls -l /usr/lib/apache2/modules/mod_cacher.so && which cacher
```

## 6. Create the cache directory

```bash
install -d -o www-data -g www-data /var/cache/cacher
```

The module creates this at startup too, but doing it explicitly gets the
ownership right the first time.

## 7. Load the module

```bash
echo "LoadModule cacher_module /usr/lib/apache2/modules/mod_cacher.so" > /etc/apache2/mods-available/cacher.load
```

## 8. Set the server-wide config

`CacherCacheRoot` is server-level only. Everything else can be per-site.

```bash
printf 'CacherCacheRoot /var/cache/cacher\nLogLevel info cacher:trace1\n' > /etc/apache2/conf-available/cacher.conf
```

Drop `cacher:trace1` once you are done testing - it logs a line per cache
decision.

## 9. Enable and restart

```bash
a2enmod cacher && a2enconf cacher && apache2ctl configtest
```

**Only restart if that prints `Syntax OK`:**

```bash
systemctl restart apache2 && apache2ctl -M | grep cacher
```

You want to see `cacher_module (shared)`.

## 10. Confirm your sites are unaffected

Nothing is cached yet - no directory has opted in. Your sites should behave
exactly as before:

```bash
for h in $(apache2ctl -S 2>/dev/null | grep -oP 'namevhost \K\S+' | sort -u); do printf "%s -> " "$h"; curl -s -o /dev/null -w "%{http_code}\n" -H "Host: $h" http://localhost/; done
```

---

## 11. Turn caching on for one site

Find the site's document root:

```bash
apache2ctl -S
```

Install a rules file. The shipped WordPress/WooCommerce example caches
rendered HTML only and excludes admin, login, REST, AJAX, cart, checkout,
account and static assets:

```bash
DOCROOT=/var/www/html/staging/gs-web-staging/src   # <-- your document root
cp /opt/cacher/examples/wordpress-woocommerce-rules.json $DOCROOT/cacher-rules.json
```

See [RULES.md](RULES.md) for writing your own.

Enable it in the site's `.htaccess`. **The leading `\n` matters** - many
`.htaccess` files have no trailing newline, and without it your first
directive is silently glued onto the previous line:

```bash
printf '\nCacherEnable On\nCacherRulesFile cacher-rules.json\n' >> $DOCROOT/.htaccess
tail -4 $DOCROOT/.htaccess
```

Check those appear as separate lines. `.htaccess` needs no reload.

The site's `<Directory>` must allow overrides - `AllowOverride All` or at
least `FileInfo`:

```bash
grep -A3 "$DOCROOT" /etc/apache2/sites-enabled/*.conf | grep -i allowoverride
```

## 12. Verify caching works

```bash
curl -s -o /dev/null https://your-site.example/; curl -s -o /dev/null https://your-site.example/; cacher list
```

`cacher list` should show the page with a size and remaining TTL. Confirm a
logged-in visitor bypasses it:

```bash
curl -s -o /dev/null -b "wordpress_logged_in_x=1" https://your-site.example/; grep cacher /var/log/apache2/<your-site>-error.log | tail -5
```

Note the log is the **vhost's** error log, not `/var/log/apache2/error.log`.

---

## 13. Optional: web-based cache control

By default the only control surface is the `cacher` CLI on the box. To
manage a site's cache over HTTP, add to that site's `.htaccess` or vhost:

```apache
CacherAdminPath /cacher-admin
```

The endpoints refuse unauthenticated requests unless you say otherwise, so
pick one:

**Open** - fine on staging, where anyone clearing the cache is harmless:

```apache
CacherAdminRequireUser Off
```

**Password protected** - for anything that matters:

```bash
htpasswd -c /etc/apache2/cacher.htpasswd cacheadmin
chown root:www-data /etc/apache2/cacher.htpasswd && chmod 640 /etc/apache2/cacher.htpasswd
```

```apache
<Location /cacher-admin>
    AuthType Basic
    AuthName "Cache admin"
    AuthUserFile /etc/apache2/cacher.htpasswd
    Require valid-user
</Location>
```

**IP allowlist** - note this leaves no authenticated user, so it also needs
`CacherAdminRequireUser Off`; Apache still enforces the allowlist:

```apache
<Location /cacher-admin>
    Require ip 203.0.113.4
</Location>
```

### WordPress and other front-controller apps

Any app that rewrites every unmatched URL to one entry point will swallow
the admin path. Add this **above** the app's own rules in `.htaccess`:

```apache
<IfModule mod_rewrite.c>
RewriteEngine On
RewriteRule ^cacher-admin - [END]
</IfModule>
```

Then:

```
https://your-site.example/cacher-admin/list
https://your-site.example/cacher-admin/purge/blog/*
https://your-site.example/cacher-admin/full-reset
```

Endpoints only ever act on entries for the site they were requested through.
Use the CLI for server-wide work.

---

## Updating later

```bash
cd /opt/cacher && git pull && make test && make && make install && systemctl restart apache2
```

## Removing it completely

```bash
a2dismod cacher; a2disconf cacher
apache2ctl configtest && systemctl restart apache2
rm -f /etc/apache2/mods-available/cacher.load /etc/apache2/conf-available/cacher.conf
rm -f /usr/lib/apache2/modules/mod_cacher.so /usr/local/bin/cacher
rm -rf /var/cache/cacher
```

Then remove the `Cacher*` lines from any site's `.htaccess`. Apache is back
to exactly its previous state.
