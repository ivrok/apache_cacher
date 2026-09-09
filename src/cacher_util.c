#include "cacher_util.h"

#include <ctype.h>
#include <string.h>

#include "apr_strings.h"

request_rec *cacher_original_request(request_rec *r)
{
    request_rec *top = r;

    while (top->prev) {
        top = top->prev;
    }
    return top;
}

int cacher_header_name_is(const char *name, const char *target)
{
    while (*name && *target) {
        if (tolower((unsigned char) *name) != tolower((unsigned char) *target)) {
            return 0;
        }
        name++;
        target++;
    }
    return *name == '\0' && *target == '\0';
}

typedef void (*cookie_cb)(const char *name, const char *value, void *ctx);

/*
 * Splits the request's "Cookie: name1=value1; name2=value2" header and
 * invokes `cb` for each pair. Operates on a per-request pool-allocated
 * copy, so name/value pointers handed to `cb` remain valid for the
 * lifetime of the request.
 */
static void for_each_cookie(request_rec *r, cookie_cb cb, void *ctx)
{
    const char *header = apr_table_get(r->headers_in, "Cookie");
    char *cookies;
    char *pair;
    char *last = NULL;

    if (!header) {
        return;
    }

    cookies = apr_pstrdup(r->pool, header);

    for (pair = apr_strtok(cookies, ";", &last); pair; pair = apr_strtok(NULL, ";", &last)) {
        char *eq;
        char *name;
        char *value;

        while (*pair == ' ') {
            pair++;
        }

        eq = strchr(pair, '=');
        if (!eq) {
            continue;
        }

        *eq = '\0';
        name = pair;
        value = eq + 1;

        cb(name, value, ctx);
    }
}

typedef struct {
    const cacher_rule *rule;
    int bypass;
} bypass_ctx;

static void bypass_cb(const char *name, const char *value, void *ctxp)
{
    bypass_ctx *ctx = ctxp;

    (void) value;
    if (!ctx->bypass && cacher_rule_bypasses_cookie(ctx->rule, name)) {
        ctx->bypass = 1;
    }
}

int cacher_request_bypasses(request_rec *r, const cacher_rule *rule)
{
    bypass_ctx ctx;

    if (!rule || !rule->bypass_cookies) {
        return 0;
    }

    ctx.rule = rule;
    ctx.bypass = 0;
    for_each_cookie(r, bypass_cb, &ctx);
    return ctx.bypass;
}

typedef struct {
    const char *name;
    const char *value;
} lookup_ctx;

static void lookup_cb(const char *name, const char *value, void *ctxp)
{
    lookup_ctx *ctx = ctxp;

    if (!ctx->value && strcmp(name, ctx->name) == 0) {
        ctx->value = value;
    }
}

const char *cacher_get_cookie(request_rec *r, const char *name)
{
    lookup_ctx ctx;

    ctx.name = name;
    ctx.value = NULL;
    for_each_cookie(r, lookup_cb, &ctx);
    return ctx.value;
}
