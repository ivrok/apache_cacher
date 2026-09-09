#ifndef CACHER_RULES_H
#define CACHER_RULES_H

#include <stddef.h>

/*
 * JSON rule parsing and matching for Cacher.
 *
 * Deliberately independent of Apache/APR headers so this file (plus
 * third_party/cJSON.c) can be built and unit-tested standalone - see
 * test/test_rules_parse.c. Only the C standard library is required.
 */

typedef struct {
    char *path;             /* glob pattern ('*'/'?'), NULL = match any path */
    char *query;            /* glob against the query string, NULL = match any */
    char **methods;         /* NULL-terminated array of uppercase methods, NULL = match any */
    int enabled;            /* 1 = cache when this rule matches (default);
                               0 = matched, but explicitly NOT cacheable */
    long ttl_seconds;       /* freshness window; <= 0 = never fresh */
    int *status_codes;      /* array terminated by -1; defaults to {200,-1} if omitted */
    char **content_types;   /* NULL-terminated globs against the response Content-Type; NULL = any */
    char **bypass_cookies;  /* NULL-terminated array of glob patterns matched against cookie NAMEs */
    char **vary;            /* NULL-terminated array: header names, or "cookie:NAME" */
} cacher_rule;

typedef struct {
    cacher_rule **rules;    /* NULL-terminated array, ordered; first match wins */
} cacher_ruleset;

/*
 * Parses `json` (schema: {"rules": [...]}) into a newly malloc'd ruleset.
 * Returns NULL on a hard JSON syntax error or a missing top-level "rules"
 * array, writing a short message into errbuf (if non-NULL). An individual
 * rule object with invalid/missing fields falls back to safe defaults
 * rather than aborting the whole parse.
 */
cacher_ruleset *cacher_ruleset_parse(const char *json, char *errbuf, size_t errbuf_len);

/* Releases everything allocated by cacher_ruleset_parse(). Safe to call with NULL. */
void cacher_ruleset_free(cacher_ruleset *rs);

/*
 * Returns the first rule whose match clause matches this request, or NULL
 * if none do. `query` and `method` may be NULL. The returned pointer is
 * owned by `rs`.
 *
 * Disabled rules take part in matching and can win: first match wins,
 * full stop. A matched rule with enabled == 0 means "this request is
 * explicitly not cacheable" - the caller must check `enabled` and stop,
 * NOT fall through to later rules. That is what makes an exclusion list
 * ahead of a catch-all rule behave the way anyone would expect.
 */
const cacher_rule *cacher_ruleset_match(const cacher_ruleset *rs, const char *path,
                                         const char *query, const char *method);

/* True if any of `rule`'s bypass_cookies patterns matches `cookie_name`. */
int cacher_rule_bypasses_cookie(const cacher_rule *rule, const char *cookie_name);

/* True if `status` is one of `rule`'s status_codes. */
int cacher_rule_allows_status(const cacher_rule *rule, int status);

/*
 * True if `content_type` matches one of `rule`'s content_types globs, or if
 * the rule sets none. Parameters are ignored, so "text/html; charset=UTF-8"
 * is tested as "text/html"; matching is case-insensitive.
 *
 * Checked on the write path only: the response type is not known when the
 * request arrives. It is the reliable way to keep static assets out of the
 * cache - Apache already serves those well, and they never reach PHP, so
 * caching them costs disk and buys nothing.
 */
int cacher_rule_allows_content_type(const cacher_rule *rule, const char *content_type);

/*
 * Simple glob match: '*' matches any run of characters, '?' matches any
 * single character, the rest must match literally. Anchored to the whole
 * string (i.e. implicit '^' and '$'). `ci` selects case-insensitive
 * matching (used for cookie names).
 */
int cacher_glob_match(const char *pattern, const char *value, int ci);

#endif /* CACHER_RULES_H */
