/*
 * Standalone unit tests for cacher_rules.c - no Apache/APR headers, only
 * the C standard library and the vendored cJSON. Build/run with:
 *
 *   cc -I../src -I../third_party -o test_rules_parse \
 *      test_rules_parse.c ../src/cacher_rules.c ../third_party/cJSON.c
 *   ./test_rules_parse
 */

#include <stdio.h>
#include <string.h>

#include "cacher_rules.h"

static int failures = 0;

#define CHECK(cond, msg) \
    do { \
        if (!(cond)) { \
            printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
            failures++; \
        } \
    } while (0)

static void test_glob_match(void)
{
    CHECK(cacher_glob_match("/blog/*", "/blog/hello-world", 0), "glob: prefix star matches");
    CHECK(!cacher_glob_match("/blog/*", "/shop/hello-world", 0), "glob: mismatched prefix rejected");
    CHECK(cacher_glob_match("*", "/anything", 0), "glob: bare star matches everything");
    CHECK(cacher_glob_match("wordpress_logged_in_*", "wordpress_logged_in_abc123", 0), "glob: cookie-name-style pattern");
    CHECK(!cacher_glob_match("wordpress_logged_in_*", "PHPSESSID", 0), "glob: unrelated cookie name rejected");
    CHECK(cacher_glob_match("PHPSESSID", "phpsessid", 1), "glob: case-insensitive exact match");
    CHECK(!cacher_glob_match("PHPSESSID", "phpsessid", 0), "glob: case-sensitive exact match rejects different case");
    CHECK(cacher_glob_match("a?c", "abc", 0), "glob: '?' matches single char");
    CHECK(!cacher_glob_match("a?c", "ac", 0), "glob: '?' requires exactly one char");
}

static void test_parse_basic(void)
{
    const char *json =
        "{\"rules\":["
        "  {\"match\":{\"path\":\"/blog/*\",\"methods\":[\"GET\",\"HEAD\"]},"
        "   \"ttl\":300,"
        "   \"status_codes\":[200],"
        "   \"bypass_cookies\":[\"PHPSESSID\",\"wordpress_logged_in_*\"],"
        "   \"vary\":[\"Accept-Encoding\",\"cookie:cart_id\"]}"
        "]}";
    char errbuf[128];
    cacher_ruleset *rs = cacher_ruleset_parse(json, errbuf, sizeof(errbuf));
    const cacher_rule *matched;

    CHECK(rs != NULL, "parse: valid ruleset parses");
    if (!rs) {
        return;
    }

    matched = cacher_ruleset_match(rs, "/blog/my-post", NULL, "GET");
    CHECK(matched != NULL, "match: GET /blog/my-post matches the blog rule");
    if (matched) {
        CHECK(matched->ttl_seconds == 300, "match: ttl parsed correctly");
        CHECK(cacher_rule_allows_status(matched, 200), "match: status 200 allowed");
        CHECK(!cacher_rule_allows_status(matched, 404), "match: status 404 not allowed");
        CHECK(cacher_rule_bypasses_cookie(matched, "PHPSESSID"), "match: PHPSESSID triggers bypass");
        CHECK(cacher_rule_bypasses_cookie(matched, "wordpress_logged_in_abc"), "match: wp cookie glob triggers bypass");
        CHECK(!cacher_rule_bypasses_cookie(matched, "cart_id"), "match: unrelated cookie does not bypass");
    }

    CHECK(cacher_ruleset_match(rs, "/shop/item", NULL, "GET") == NULL, "match: /shop/item does not match /blog/* rule");
    CHECK(cacher_ruleset_match(rs, "/blog/my-post", NULL, "POST") == NULL, "match: POST excluded by methods list");

    cacher_ruleset_free(rs);
}

static void test_parse_defaults(void)
{
    /* No "match", no "status_codes", no "enabled" - everything should fall
     * back to sane defaults (match any path/method, default status {200},
     * enabled=1) rather than erroring. */
    const char *json = "{\"rules\":[{\"ttl\":60}]}";
    char errbuf[128];
    cacher_ruleset *rs = cacher_ruleset_parse(json, errbuf, sizeof(errbuf));
    const cacher_rule *matched;

    CHECK(rs != NULL, "defaults: parses with only ttl set");
    if (!rs) {
        return;
    }

    matched = cacher_ruleset_match(rs, "/any/path/at/all", NULL, "DELETE");
    CHECK(matched != NULL, "defaults: rule with no match clause matches everything");
    if (matched) {
        CHECK(cacher_rule_allows_status(matched, 200), "defaults: status_codes defaults to {200}");
        CHECK(!cacher_rule_allows_status(matched, 301), "defaults: 301 not implicitly allowed");
    }

    cacher_ruleset_free(rs);
}

/*
 * The exclusion-list-then-catch-all pattern. A disabled rule must WIN when
 * it matches, not be passed over in favour of a later catch-all - getting
 * this backwards silently caches precisely what the exclusion was written
 * to protect.
 */
static void test_disabled_rule_excludes_rather_than_falls_through(void)
{
    const char *json =
        "{\"rules\":["
        "  {\"match\":{\"path\":\"/admin/*\"},\"enabled\":false},"
        "  {\"ttl\":10}"
        "]}";
    char errbuf[128];
    cacher_ruleset *rs = cacher_ruleset_parse(json, errbuf, sizeof(errbuf));
    const cacher_rule *matched;

    CHECK(rs != NULL, "exclusion: ruleset parses");
    if (!rs) {
        return;
    }

    matched = cacher_ruleset_match(rs, "/admin/dashboard", NULL, "GET");
    CHECK(matched != NULL, "exclusion: /admin/* matches its own rule");
    if (matched) {
        CHECK(matched->enabled == 0, "exclusion: the disabled rule wins, NOT the catch-all");
    }

    matched = cacher_ruleset_match(rs, "/blog/post", NULL, "GET");
    CHECK(matched != NULL, "exclusion: an unrelated path still reaches the catch-all");
    if (matched) {
        CHECK(matched->enabled == 1, "exclusion: catch-all is enabled");
        CHECK(matched->ttl_seconds == 10, "exclusion: catch-all ttl applies");
    }

    cacher_ruleset_free(rs);
}

/* WooCommerce AJAX lives entirely in the query string (/?wc-ajax=...), so
 * excluding it requires query matching - the path alone is just "/". */
static void test_query_matching(void)
{
    const char *json =
        "{\"rules\":["
        "  {\"match\":{\"query\":\"*wc-ajax=*\"},\"enabled\":false},"
        "  {\"ttl\":60}"
        "]}";
    char errbuf[128];
    cacher_ruleset *rs = cacher_ruleset_parse(json, errbuf, sizeof(errbuf));
    const cacher_rule *matched;

    CHECK(rs != NULL, "query: ruleset parses");
    if (!rs) {
        return;
    }

    matched = cacher_ruleset_match(rs, "/", "wc-ajax=get_refreshed_fragments", "GET");
    CHECK(matched && matched->enabled == 0, "query: wc-ajax request hits the exclusion");

    matched = cacher_ruleset_match(rs, "/", NULL, "GET");
    CHECK(matched && matched->enabled == 1, "query: same path with no query is cacheable");

    matched = cacher_ruleset_match(rs, "/", "utm_source=news", "GET");
    CHECK(matched && matched->enabled == 1, "query: unrelated query string is cacheable");

    cacher_ruleset_free(rs);
}

static void test_parse_errors(void)
{
    char errbuf[128];

    CHECK(cacher_ruleset_parse("{not json", errbuf, sizeof(errbuf)) == NULL, "errors: malformed JSON rejected");
    CHECK(cacher_ruleset_parse("{\"nope\":[]}", errbuf, sizeof(errbuf)) == NULL, "errors: missing \"rules\" array rejected");
    CHECK(cacher_ruleset_parse(NULL, errbuf, sizeof(errbuf)) == NULL, "errors: NULL input rejected");
}

int main(void)
{
    test_glob_match();
    test_parse_basic();
    test_parse_defaults();
    test_disabled_rule_excludes_rather_than_falls_through();
    test_query_matching();
    test_parse_errors();

    if (failures == 0) {
        printf("All tests passed.\n");
        return 0;
    }

    printf("%d test(s) failed.\n", failures);
    return 1;
}
