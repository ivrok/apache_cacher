#ifndef CACHER_CONFIG_H
#define CACHER_CONFIG_H

#include "httpd.h"
#include "http_config.h"

#include "cacher_rules.h"

/* Tri-state sentinel for directives not yet set at this config level. */
#define CACHER_UNSET (-1)

/* Per-directory config, built up via .htaccess (or <Directory>/<Location>). */
typedef struct {
    int enabled;             /* CACHER_UNSET / 0 (off) / 1 (on) */
    const char *rules_json;  /* CacherRules inline value, or NULL */
    const char *rules_file;  /* CacherRulesFile value, or NULL; wins over rules_json */
    const char *config_dir;  /* Directory this config level was created for; base for
                                 resolving a relative rules_file. */
} cacher_dir_conf;

/* Per-server config. Both directives are RSRC_CONF only: allowing them in
 * .htaccess would let anyone able to write into a document root redirect
 * the cache or expose its admin endpoints. */
typedef struct {
    const char *cache_root;
    const char *admin_path;  /* CacherAdminPath, or NULL = endpoints disabled */
} cacher_svr_conf;

extern module AP_MODULE_DECLARE_DATA cacher_module;

void *cacher_create_dir_config(apr_pool_t *p, char *dir);
void *cacher_merge_dir_config(apr_pool_t *p, void *basev, void *newv);
void *cacher_create_server_config(apr_pool_t *p, server_rec *s);
void *cacher_merge_server_config(apr_pool_t *p, void *basev, void *newv);

extern const command_rec cacher_cmds[];

/* Must be called once from the child_init hook before any request uses
 * cacher_config_get_ruleset() - sets up the process-local mutex guarding
 * the parsed-rules-file cache. */
void cacher_config_child_init(apr_pool_t *pchild, server_rec *s);

/*
 * Returns the ruleset that applies to this request's per-directory config,
 * or NULL if Cacher has nothing usable configured for this directory
 * (disabled, no rules set, or a parse error - logged via ap_log_rerror).
 *
 * CacherRulesFile results are cached process-wide keyed by (resolved path,
 * mtime, size): the file is only re-read/re-parsed when it actually
 * changes on disk. CacherRules (inline) is small and re-parsed on every
 * call - cheap since there's no file I/O involved.
 *
 * Do not free the returned pointer: a CacherRulesFile ruleset lives in the
 * process-wide cache and is reused by later requests; an inline CacherRules
 * ruleset is tied to this request's pool and freed automatically when the
 * request ends.
 */
const cacher_ruleset *cacher_config_get_ruleset(request_rec *r, const cacher_dir_conf *dconf);

#endif /* CACHER_CONFIG_H */
