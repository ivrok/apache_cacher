# Writing cache rules

Rules are JSON, live next to the site they apply to, and take effect
immediately - no Apache reload.

```apache
CacherEnable On
CacherRulesFile cacher-rules.json
```

`CacherRulesFile` is resolved relative to the directory holding the
`.htaccess`.

**An absolute path is usually the better choice**, because the rules are
server configuration rather than site content and have no business sitting
in a deployable tree - a deploy that cleans untracked files will delete
them, and a missing rules file makes the module decline silently:

```apache
CacherRulesFile /etc/cacher/example.com.json
```

Inline rules also work for short ones, in **single quotes** so the JSON's
own double quotes survive:

```apache
CacherRules '{"rules":[{"ttl":300,"content_types":["text/html"]}]}'
```

---

## The one rule that matters

**Rules are ordered, and the first match wins - exclusions included.**

Every `"enabled": false` rule must sit *above* your catch-all. A rule placed
after it is unreachable, because the catch-all matches first.

Think of it as: exclusions at the top, the thing you actually want to cache
at the bottom.

If nothing matches, the request is not cached.

---

## Example 1: the smallest useful rule

Cache every `GET`/`HEAD` HTML page for 5 minutes.

```json
{
  "rules": [
    {
      "match": { "methods": ["GET", "HEAD"] },
      "ttl": 300,
      "content_types": ["text/html"]
    }
  ]
}
```

`content_types` matters more than it looks. Without it, every stylesheet,
script, font and image gets stored too - Apache already serves those well,
they never reach PHP, and they will dominate your cache.

## Example 2: exclude a section

```json
{
  "rules": [
    { "match": { "path": "/admin/*" }, "enabled": false },
    { "match": { "path": "/account/*" }, "enabled": false },

    {
      "match": { "methods": ["GET", "HEAD"] },
      "ttl": 300,
      "content_types": ["text/html"]
    }
  ]
}
```

## Example 3: don't cache for logged-in visitors

`bypass_cookies` is matched against cookie **names**, never values, and
supports `*`.

```json
{
  "rules": [
    {
      "match": { "methods": ["GET", "HEAD"] },
      "ttl": 300,
      "content_types": ["text/html"],
      "bypass_cookies": ["session_*", "auth_token"]
    }
  ]
}
```

A request carrying any matching cookie is never served from the cache and
never stored into it.

**Pick these carefully.** If you list a cookie your site sets for *everyone*
- `PHPSESSID` is the classic trap - then every real browser bypasses after
its first page view and the cache serves nobody. Check what an anonymous
visitor actually receives:

```bash
curl -s -D - -o /dev/null https://your-site.example/ | grep -i set-cookie
```

## Example 4: different TTLs per section

Fast-moving pages short, stable pages long.

```json
{
  "rules": [
    {
      "match": { "path": "/news/*", "methods": ["GET", "HEAD"] },
      "ttl": 60,
      "content_types": ["text/html"]
    },
    {
      "match": { "path": "/docs/*", "methods": ["GET", "HEAD"] },
      "ttl": 86400,
      "content_types": ["text/html"]
    },
    {
      "match": { "methods": ["GET", "HEAD"] },
      "ttl": 300,
      "content_types": ["text/html"]
    }
  ]
}
```

## Example 5: exclude URLs with query strings

The query string is part of the cache key, so `?utm_source=a` and
`?utm_source=b` are separate entries. One marketing campaign can multiply
your cache. `"?*"` means "at least one character":

```json
{
  "rules": [
    { "match": { "query": "?*" }, "enabled": false },

    {
      "match": { "methods": ["GET", "HEAD"] },
      "ttl": 300,
      "content_types": ["text/html"]
    }
  ]
}
```

To keep one useful query URL, allow it *above* the exclusion:

```json
{
  "rules": [
    {
      "match": { "query": "page=*" },
      "ttl": 300,
      "content_types": ["text/html"]
    },
    { "match": { "query": "?*" }, "enabled": false },

    { "match": { "methods": ["GET", "HEAD"] }, "ttl": 300, "content_types": ["text/html"] }
  ]
}
```

## Example 6: one cache entry per language

`vary` splits the cache by a request header or cookie value.

```json
{
  "rules": [
    {
      "match": { "methods": ["GET", "HEAD"] },
      "ttl": 300,
      "content_types": ["text/html"],
      "vary": ["Accept-Language", "cookie:site_lang"]
    }
  ]
}
```

Use it sparingly: each distinct value creates its own entry, so varying on
something high-cardinality (a user id, a session) multiplies the cache and
gains nothing.

---

## Field reference

| Field | Meaning |
|---|---|
| `match.path` | Glob against the request path, no query string. `*` = any run, `?` = one character. Omit to match any path. |
| `match.query` | Glob against the query string alone. Needed for anything living only in the query, e.g. `"*wc-ajax=*"`. Omit to match any. |
| `match.methods` | Array of HTTP methods. Omit to match any. |
| `enabled` | `false` makes the rule an **exclusion**: if it matches first, the request is not cached and later rules are not consulted. Default `true`. |
| `ttl` | Seconds an entry stays fresh. `0` or absent means never fresh. |
| `status_codes` | Response statuses eligible for caching. Default `[200]`. |
| `content_types` | Globs against the response `Content-Type`; `; charset=…` is ignored and matching is case-insensitive. Omit to accept any type. |
| `bypass_cookies` | Globs against cookie **names**. Any match means never serve from, and never store into, the cache. |
| `vary` | Header names, plus optional `cookie:<name>`, that partition the cache. |

Unknown fields are ignored, so `"_comment"` is a convenient way to annotate
a rules file.

---

## Is it actually caching? Read the header

Every response from a Cacher-enabled directory carries `X-Cacher`:

| Value | Meaning |
|---|---|
| `HIT` | served from the cache; an `Age` header gives its age in seconds |
| `MISS` | generated now, and stored for next time |
| `BYPASS` | a `bypass_cookies` match - never cached for this visitor |
| `EXCLUDED` | matched a rule with `"enabled": false` |
| `DYNAMIC` | Cacher is on here, but no rule matched |

```bash
curl -sI https://your-site.example/some-page/ | grep -i '^x-cacher\|^age'
```

**Test with `curl`, not your browser.** A browser logged into the site
sends its session cookies, so it will correctly get `BYPASS` on every
request - which looks exactly like "caching is broken". `curl` sends no
cookies unless told to.

Turn the header off with `CacherStatusHeader Off`.

## Checking your work

Validate the JSON before wondering why nothing caches:

```bash
python3 -m json.tool cacher-rules.json >/dev/null && echo ok
```

Watch the decisions, one line per request, in the **vhost's** error log with
`LogLevel info cacher:trace1`:

```bash
grep cacher /var/log/apache2/<your-site>-error.log | tail -20
```

You will see `served from cache`, `matched no rule`, `matched an exclusion
rule`, or `bypassed (matching cookie present)` - which usually tells you
immediately which rule is or isn't firing.

Then look at what actually landed:

```bash
cacher list
```

If that shows CSS, JS or images, your catch-all needs `content_types`. If it
shows one entry where you expected several distinct pages, see
[TROUBLESHOOTING.md](TROUBLESHOOTING.md).

---

## A worked example

[`examples/wordpress-woocommerce-rules.json`](../examples/wordpress-woocommerce-rules.json)
is a production-shaped WordPress + WooCommerce config: static assets and
admin, login, cron, XML-RPC, REST, cart, checkout, account and AJAX paths
excluded, query strings excluded, and a catch-all that caches rendered HTML
for five minutes and bypasses on the WordPress and WooCommerce
personalisation cookies.
