#include "cacher_config.h"

#include <string.h>

#include "apr_file_info.h"
#include "apr_file_io.h"
#include "apr_strings.h"
#include "apr_thread_mutex.h"
#include "http_log.h"

/* Without this, aplog_module_index stays NULL in this translation unit and
 * APLOG_MARK reports APLOG_NO_MODULE - meaning "LogLevel cacher:trace1"
 * would silently not apply to anything logged from this file. */
APLOG_USE_MODULE(cacher);

/*
 * Process-wide cache of parsed CacherRulesFile rulesets, keyed by resolved
 * path, so a rules file is only re-read/re-parsed when its mtime or size
 * actually changes - not on every request (see cacher_config_get_ruleset).
 * Guarded by rules_cache_mutex since worker/event MPMs run multiple
 * request-handling threads per process.
 */
typedef struct cacher_rules_cache_entry {
    char *path;
    apr_time_t mtime;
    apr_off_t size;
    cacher_ruleset *ruleset;
    struct cacher_rules_cache_entry *next;
} cacher_rules_cache_entry;

static apr_pool_t *rules_cache_pool = NULL;
static apr_thread_mutex_t *rules_cache_mutex = NULL;
static cacher_rules_cache_entry *rules_cache_head = NULL;

void *cacher_create_dir_config(apr_pool_t *p, char *dir)
{
    cacher_dir_conf *conf = apr_pcalloc(p, sizeof(*conf));
    conf->enabled = CACHER_UNSET;
    conf->rules_json = NULL;
    conf->rules_file = NULL;
    conf->config_dir = dir ? apr_pstrdup(p, dir) : NULL;
    return conf;
}

void *cacher_merge_dir_config(apr_pool_t *p, void *basev, void *newv)
{
    cacher_dir_conf *base = basev;
    cacher_dir_conf *add = newv;
    cacher_dir_conf *merged = apr_pcalloc(p, sizeof(*merged));

    merged->enabled = (add->enabled != CACHER_UNSET) ? add->enabled : base->enabled;
    merged->rules_file = add->rules_file ? add->rules_file : base->rules_file;
    /* CacherRulesFile always wins over CacherRules once set anywhere in the chain. */
    merged->rules_json = merged->rules_file ? NULL
                        : (add->rules_json ? add->rules_json : base->rules_json);
    /* `add` is the config freshly built for the directory currently being
     * processed - the correct base for resolving a relative rules_file
     * that was set at that level. */
    merged->config_dir = add->config_dir ? add->config_dir : base->config_dir;

    return merged;
}

void *cacher_create_server_config(apr_pool_t *p, server_rec *s)
{
    cacher_svr_conf *conf = apr_pcalloc(p, sizeof(*conf));

    (void) s;
    conf->cache_root = NULL;
    return conf;
}

void *cacher_merge_server_config(apr_pool_t *p, void *basev, void *newv)
{
    cacher_svr_conf *base = basev;
    cacher_svr_conf *add = newv;
    cacher_svr_conf *merged = apr_pcalloc(p, sizeof(*merged));

    merged->cache_root = add->cache_root ? add->cache_root : base->cache_root;
    return merged;
}

static const char *cacher_set_enable(cmd_parms *cmd, void *dconf, int flag)
{
    cacher_dir_conf *conf = dconf;

    (void) cmd;
    conf->enabled = flag ? 1 : 0;
    return NULL;
}

static const char *cacher_set_rules(cmd_parms *cmd, void *dconf, const char *args)
{
    cacher_dir_conf *conf = dconf;
    conf->rules_json = apr_pstrdup(cmd->pool, args);
    return NULL;
}

static const char *cacher_set_rules_file(cmd_parms *cmd, void *dconf, const char *arg)
{
    cacher_dir_conf *conf = dconf;
    conf->rules_file = apr_pstrdup(cmd->pool, arg);
    return NULL;
}

static const char *cacher_set_cache_root(cmd_parms *cmd, void *dconf, const char *arg)
{
    cacher_svr_conf *conf = ap_get_module_config(cmd->server->module_config,
                                                  &cacher_module);

    (void) dconf;
    conf->cache_root = apr_pstrdup(cmd->pool, arg);
    return NULL;
}

const command_rec cacher_cmds[] = {
    AP_INIT_FLAG("CacherEnable", cacher_set_enable, NULL, OR_FILEINFO,
                 "Enable Cacher for this directory (On|Off, default Off)"),
    AP_INIT_RAW_ARGS("CacherRules", cacher_set_rules, NULL, OR_FILEINFO,
                      "Inline JSON caching rules for this directory"),
    AP_INIT_TAKE1("CacherRulesFile", cacher_set_rules_file, NULL, OR_FILEINFO,
                  "Path to a JSON rules file, relative to this directory"),
    AP_INIT_TAKE1("CacherCacheRoot", cacher_set_cache_root, NULL, RSRC_CONF,
                  "Filesystem root for cached responses"),
    { NULL }
};

void cacher_config_child_init(apr_pool_t *pchild, server_rec *s)
{
    apr_status_t rv;

    rules_cache_pool = pchild;
    rules_cache_head = NULL;

    rv = apr_thread_mutex_create(&rules_cache_mutex, APR_THREAD_MUTEX_DEFAULT, pchild);
    if (rv != APR_SUCCESS) {
        ap_log_error(APLOG_MARK, APLOG_ERR, rv, s,
                      "cacher: could not create rules cache mutex - "
                      "CacherRulesFile will be unusable in this child process");
    }
}

static char *resolve_rules_file_path(request_rec *r, const cacher_dir_conf *dconf)
{
    char *resolved = NULL;

    if (!dconf->config_dir) {
        ap_log_rerror(APLOG_MARK, APLOG_WARNING, 0, r,
                      "cacher: CacherRulesFile '%s' set in a context with no "
                      "directory to resolve it against (e.g. <Location>) - ignored",
                      dconf->rules_file);
        return NULL;
    }

    if (apr_filepath_merge(&resolved, dconf->config_dir, dconf->rules_file,
                            0, r->pool) != APR_SUCCESS) {
        ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r,
                      "cacher: could not resolve CacherRulesFile '%s' against "
                      "directory '%s'", dconf->rules_file, dconf->config_dir);
        return NULL;
    }

    return resolved;
}

static apr_status_t cleanup_ruleset(void *data)
{
    cacher_ruleset_free((cacher_ruleset *) data);
    return APR_SUCCESS;
}

/* Reads and parses dconf->rules_file, using the process-wide mtime/size
 * cache so unchanged files are never re-read. */
static const cacher_ruleset *get_ruleset_from_file(request_rec *r, const cacher_dir_conf *dconf)
{
    char *path;
    apr_finfo_t finfo;
    cacher_rules_cache_entry *entry;
    cacher_ruleset *fresh;
    apr_file_t *f;
    char *buf;
    apr_size_t nbytes;
    char errbuf[256];

    if (!rules_cache_mutex) {
        ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r,
                      "cacher: rules cache mutex unavailable, CacherRulesFile disabled");
        return NULL;
    }

    path = resolve_rules_file_path(r, dconf);
    if (!path) {
        return NULL;
    }

    if (apr_stat(&finfo, path, APR_FINFO_MTIME | APR_FINFO_SIZE, r->pool) != APR_SUCCESS) {
        ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r,
                      "cacher: CacherRulesFile '%s' not found", path);
        return NULL;
    }

    apr_thread_mutex_lock(rules_cache_mutex);
    for (entry = rules_cache_head; entry; entry = entry->next) {
        if (strcmp(entry->path, path) == 0) {
            break;
        }
    }
    if (entry && entry->mtime == finfo.mtime && entry->size == finfo.size) {
        cacher_ruleset *cached = entry->ruleset;
        apr_thread_mutex_unlock(rules_cache_mutex);
        if (!cached) {
            ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r,
                          "cacher: CacherRulesFile '%s' previously failed to parse", path);
        }
        return cached;
    }
    apr_thread_mutex_unlock(rules_cache_mutex);

    /* Cache miss - the file is new or changed. This only happens on an
     * operator edit, not on the request hot path. */
    if (apr_file_open(&f, path, APR_FOPEN_READ, APR_FPROT_OS_DEFAULT, r->pool) != APR_SUCCESS) {
        ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r,
                      "cacher: could not open CacherRulesFile '%s'", path);
        return NULL;
    }

    nbytes = (apr_size_t) finfo.size;
    buf = apr_palloc(r->pool, nbytes + 1);
    if (apr_file_read_full(f, buf, nbytes, NULL) != APR_SUCCESS) {
        apr_file_close(f);
        ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r,
                      "cacher: could not read CacherRulesFile '%s'", path);
        return NULL;
    }
    apr_file_close(f);
    buf[nbytes] = '\0';

    fresh = cacher_ruleset_parse(buf, errbuf, sizeof(errbuf));
    if (!fresh) {
        ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r,
                      "cacher: failed to parse CacherRulesFile '%s': %s", path, errbuf);
    }

    apr_thread_mutex_lock(rules_cache_mutex);
    for (entry = rules_cache_head; entry; entry = entry->next) {
        if (strcmp(entry->path, path) == 0) {
            break;
        }
    }
    if (!entry) {
        entry = apr_pcalloc(rules_cache_pool, sizeof(*entry));
        entry->path = apr_pstrdup(rules_cache_pool, path);
        entry->next = rules_cache_head;
        rules_cache_head = entry;
    }
    /* Deliberately not freeing entry->ruleset here even when replacing it:
     * another thread may still hold a pointer to the old ruleset from a
     * request that's mid-flight. Rules files change on rare operator
     * edits, not per-request, so the old generation leaking until process
     * restart is a bounded, acceptable tradeoff against a use-after-free. */
    entry->mtime = finfo.mtime;
    entry->size = finfo.size;
    entry->ruleset = fresh;
    apr_thread_mutex_unlock(rules_cache_mutex);

    return fresh;
}

const cacher_ruleset *cacher_config_get_ruleset(request_rec *r, const cacher_dir_conf *dconf)
{
    if (!dconf) {
        return NULL;
    }

    if (dconf->rules_file) {
        return get_ruleset_from_file(r, dconf);
    }

    if (dconf->rules_json) {
        /* .htaccess is re-parsed every request regardless, so this string
         * is already fresh; re-parsing a short inline snippet here is far
         * cheaper than the disk I/O the file-backed path above avoids.
         * Tie the parsed ruleset's lifetime to the request pool so it's
         * freed automatically at the end of the request. */
        char errbuf[256];
        cacher_ruleset *fresh = cacher_ruleset_parse(dconf->rules_json, errbuf, sizeof(errbuf));

        if (!fresh) {
            ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r,
                          "cacher: failed to parse CacherRules: %s", errbuf);
            return NULL;
        }
        apr_pool_cleanup_register(r->pool, fresh, cleanup_ruleset, apr_pool_cleanup_null);
        return fresh;
    }

    return NULL;
}
