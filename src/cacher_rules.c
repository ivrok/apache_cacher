#include "cacher_rules.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

static char *cacher_strdup(const char *s)
{
    size_t len;
    char *out;

    if (!s) {
        return NULL;
    }

    len = strlen(s) + 1;
    out = malloc(len);
    if (out) {
        memcpy(out, s, len);
    }
    return out;
}

/* Portable case-insensitive compare - avoids strcasecmp (POSIX) vs
 * _stricmp (MSVC) split. */
static int cacher_strcasecmp(const char *a, const char *b)
{
    while (*a && *b) {
        int ca = tolower((unsigned char) *a);
        int cb = tolower((unsigned char) *b);
        if (ca != cb) {
            return ca - cb;
        }
        a++;
        b++;
    }
    return (unsigned char) *a - (unsigned char) *b;
}

int cacher_glob_match(const char *pattern, const char *value, int ci)
{
    const char *p = pattern;
    const char *v = value;
    const char *star_p = NULL;
    const char *star_v = NULL;

    if (!pattern || !value) {
        return 0;
    }

    while (*v) {
        int pc = (unsigned char) *p;
        int vc = (unsigned char) *v;

        if (ci) {
            pc = tolower(pc);
            vc = tolower(vc);
        }

        if (*p == '?' || (*p && pc == vc)) {
            p++;
            v++;
        } else if (*p == '*') {
            star_p = p++;
            star_v = v;
        } else if (star_p) {
            p = star_p + 1;
            v = ++star_v;
        } else {
            return 0;
        }
    }

    while (*p == '*') {
        p++;
    }
    return *p == '\0';
}

static void free_string_array(char **arr)
{
    int i;

    if (!arr) {
        return;
    }
    for (i = 0; arr[i]; i++) {
        free(arr[i]);
    }
    free(arr);
}

/* Builds a NULL-terminated array of (optionally uppercased) strings from a
 * cJSON string array. Returns NULL if `arr` isn't a non-empty array of
 * strings. */
static char **json_string_array(const cJSON *arr, int to_upper)
{
    int count;
    int i;
    char **out;
    const cJSON *item;

    if (!arr || !cJSON_IsArray(arr)) {
        return NULL;
    }

    count = cJSON_GetArraySize(arr);
    if (count <= 0) {
        return NULL;
    }

    out = calloc((size_t) count + 1, sizeof(char *));
    if (!out) {
        return NULL;
    }

    i = 0;
    cJSON_ArrayForEach(item, arr) {
        char *c;

        if (!cJSON_IsString(item) || !item->valuestring) {
            continue;
        }
        out[i] = cacher_strdup(item->valuestring);
        if (!out[i]) {
            continue;
        }
        if (to_upper) {
            for (c = out[i]; *c; c++) {
                *c = (char) toupper((unsigned char) *c);
            }
        }
        i++;
    }
    out[i] = NULL;

    if (i == 0) {
        free(out);
        return NULL;
    }
    return out;
}

/* Builds an array of ints terminated by -1 (a safe sentinel: HTTP status
 * codes are always positive) from a cJSON number array. */
static int *json_int_array(const cJSON *arr)
{
    int count;
    int n = 0;
    int *out;
    const cJSON *item;

    if (!arr || !cJSON_IsArray(arr)) {
        return NULL;
    }

    count = cJSON_GetArraySize(arr);
    if (count <= 0) {
        return NULL;
    }

    out = malloc(((size_t) count + 1) * sizeof(int));
    if (!out) {
        return NULL;
    }

    cJSON_ArrayForEach(item, arr) {
        if (cJSON_IsNumber(item)) {
            out[n++] = (int) item->valuedouble;
        }
    }
    out[n] = -1;

    if (n == 0) {
        free(out);
        return NULL;
    }
    return out;
}

static void free_rule(cacher_rule *r)
{
    if (!r) {
        return;
    }
    free(r->path);
    free(r->query);
    free_string_array(r->methods);
    free(r->status_codes);
    free_string_array(r->content_types);
    free_string_array(r->bypass_cookies);
    free_string_array(r->vary);
    free(r);
}

static cacher_rule *parse_rule(const cJSON *robj)
{
    cacher_rule *r;
    const cJSON *match;
    const cJSON *enabled;
    const cJSON *ttl;
    const cJSON *status_codes;
    const cJSON *bypass_cookies;
    const cJSON *vary;

    r = calloc(1, sizeof(*r));
    if (!r) {
        return NULL;
    }
    r->enabled = 1;

    match = cJSON_GetObjectItemCaseSensitive(robj, "match");
    if (cJSON_IsObject(match)) {
        const cJSON *path = cJSON_GetObjectItemCaseSensitive(match, "path");
        const cJSON *query = cJSON_GetObjectItemCaseSensitive(match, "query");
        const cJSON *methods = cJSON_GetObjectItemCaseSensitive(match, "methods");

        if (cJSON_IsString(path) && path->valuestring) {
            r->path = cacher_strdup(path->valuestring);
        }
        if (cJSON_IsString(query) && query->valuestring) {
            r->query = cacher_strdup(query->valuestring);
        }
        r->methods = json_string_array(methods, 1);
    }

    enabled = cJSON_GetObjectItemCaseSensitive(robj, "enabled");
    if (cJSON_IsBool(enabled)) {
        r->enabled = cJSON_IsTrue(enabled) ? 1 : 0;
    }

    ttl = cJSON_GetObjectItemCaseSensitive(robj, "ttl");
    if (cJSON_IsNumber(ttl)) {
        r->ttl_seconds = (long) ttl->valuedouble;
    }

    status_codes = cJSON_GetObjectItemCaseSensitive(robj, "status_codes");
    r->status_codes = json_int_array(status_codes);
    if (!r->status_codes) {
        /* Default: only cache plain 200 responses unless told otherwise. */
        r->status_codes = malloc(2 * sizeof(int));
        if (r->status_codes) {
            r->status_codes[0] = 200;
            r->status_codes[1] = -1;
        }
    }

    r->content_types = json_string_array(
        cJSON_GetObjectItemCaseSensitive(robj, "content_types"), 0);

    bypass_cookies = cJSON_GetObjectItemCaseSensitive(robj, "bypass_cookies");
    r->bypass_cookies = json_string_array(bypass_cookies, 0);

    vary = cJSON_GetObjectItemCaseSensitive(robj, "vary");
    r->vary = json_string_array(vary, 0);

    return r;
}

cacher_ruleset *cacher_ruleset_parse(const char *json, char *errbuf, size_t errbuf_len)
{
    cJSON *root;
    cJSON *rules_arr;
    cJSON *ritem;
    cacher_ruleset *rs;
    int count;
    int i;

    if (!json) {
        if (errbuf && errbuf_len) {
            snprintf(errbuf, errbuf_len, "no JSON supplied");
        }
        return NULL;
    }

    root = cJSON_Parse(json);
    if (!root) {
        if (errbuf && errbuf_len) {
            const char *ep = cJSON_GetErrorPtr();
            snprintf(errbuf, errbuf_len, "JSON syntax error near: %.40s",
                     ep ? ep : "(unknown)");
        }
        return NULL;
    }

    rules_arr = cJSON_GetObjectItemCaseSensitive(root, "rules");
    if (!cJSON_IsArray(rules_arr)) {
        if (errbuf && errbuf_len) {
            snprintf(errbuf, errbuf_len, "missing top-level \"rules\" array");
        }
        cJSON_Delete(root);
        return NULL;
    }

    rs = calloc(1, sizeof(*rs));
    if (!rs) {
        cJSON_Delete(root);
        return NULL;
    }

    count = cJSON_GetArraySize(rules_arr);
    rs->rules = calloc((size_t) count + 1, sizeof(cacher_rule *));
    if (!rs->rules) {
        free(rs);
        cJSON_Delete(root);
        return NULL;
    }

    i = 0;
    cJSON_ArrayForEach(ritem, rules_arr) {
        if (cJSON_IsObject(ritem)) {
            cacher_rule *r = parse_rule(ritem);
            if (r) {
                rs->rules[i++] = r;
            }
        }
    }
    rs->rules[i] = NULL;

    cJSON_Delete(root);
    return rs;
}

void cacher_ruleset_free(cacher_ruleset *rs)
{
    int i;

    if (!rs) {
        return;
    }
    if (rs->rules) {
        for (i = 0; rs->rules[i]; i++) {
            free_rule(rs->rules[i]);
        }
        free(rs->rules);
    }
    free(rs);
}

const cacher_rule *cacher_ruleset_match(const cacher_ruleset *rs, const char *path,
                                         const char *query, const char *method)
{
    int i;

    if (!rs || !rs->rules) {
        return NULL;
    }

    for (i = 0; rs->rules[i]; i++) {
        const cacher_rule *r = rs->rules[i];

        /* Disabled rules deliberately take part in matching: a matched
         * rule with enabled == 0 means "explicitly not cacheable", and
         * the caller stops there. Skipping them here instead would let an
         * exclusion fall through to a later catch-all and cache exactly
         * what the exclusion existed to protect. */

        if (r->path && !cacher_glob_match(r->path, path ? path : "", 0)) {
            continue;
        }

        if (r->query && !cacher_glob_match(r->query, query ? query : "", 0)) {
            continue;
        }

        if (r->methods) {
            int matched = 0;
            int j;

            for (j = 0; r->methods[j]; j++) {
                if (method && cacher_strcasecmp(r->methods[j], method) == 0) {
                    matched = 1;
                    break;
                }
            }
            if (!matched) {
                continue;
            }
        }

        return r;
    }

    return NULL;
}

int cacher_rule_bypasses_cookie(const cacher_rule *rule, const char *cookie_name)
{
    int i;

    if (!rule || !rule->bypass_cookies || !cookie_name) {
        return 0;
    }
    for (i = 0; rule->bypass_cookies[i]; i++) {
        if (cacher_glob_match(rule->bypass_cookies[i], cookie_name, 0)) {
            return 1;
        }
    }
    return 0;
}

int cacher_rule_allows_content_type(const cacher_rule *rule, const char *content_type)
{
    char base[128];
    size_t i = 0;
    int j;

    if (!rule || !rule->content_types) {
        return 1; /* no constraint */
    }
    if (!content_type) {
        return 0;
    }

    /* "text/html; charset=UTF-8" -> "text/html" */
    while (content_type[i] && content_type[i] != ';' && content_type[i] != ' '
           && i < sizeof(base) - 1) {
        base[i] = content_type[i];
        i++;
    }
    base[i] = '\0';

    for (j = 0; rule->content_types[j]; j++) {
        if (cacher_glob_match(rule->content_types[j], base, 1)) {
            return 1;
        }
    }
    return 0;
}

int cacher_rule_allows_status(const cacher_rule *rule, int status)
{
    int i;

    if (!rule || !rule->status_codes) {
        return 0;
    }
    for (i = 0; rule->status_codes[i] != -1; i++) {
        if (rule->status_codes[i] == status) {
            return 1;
        }
    }
    return 0;
}
