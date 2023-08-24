#include <pthread.h>
#include <sys/types.h>
#include <dirent.h>
#ifdef __APPLE__
#include <sys/param.h>
#include <sys/sysctl.h>
/* Really, Apple? */
#undef MAX
#undef MIN
#undef ALIGN
#else
#include <sys/sysinfo.h>
#endif
#include <libgen.h>

#include <yed/plugin.h>
#include <yed/syntax.h>
#undef DBG

#define THREADPOOL_IMPLEMENTATION
#include "threadpool.h"

#define JULE_IMPL
#include "jule.h"

#include "hash_table.h"


#define DBG_LOG_ON

#define LOG__XSTR(x) #x
#define LOG_XSTR(x) LOG__XSTR(x)

#define LOG(...)                                                   \
do {                                                               \
    LOG_FN_ENTER();                                                \
    yed_log(__VA_ARGS__);                                          \
    LOG_EXIT();                                                    \
} while (0)

#define ELOG(...)                                                  \
do {                                                               \
    LOG_FN_ENTER();                                                \
    yed_log("[!] " __VA_ARGS__);                                   \
    LOG_EXIT();                                                    \
} while (0)

#ifdef DBG_LOG_ON
#define DBG(...)                                                   \
do {                                                               \
    if (yed_var_is_truthy("crapport-debug-log")) {                 \
        LOG_FN_ENTER();                                            \
        yed_log(__FILE__ ":" LOG_XSTR(__LINE__) ": " __VA_ARGS__); \
        LOG_EXIT();                                                \
    }                                                              \
} while (0)
#else
#define DBG(...) ;
#endif


#define DEFAULT_CRAPPORT_DIR     ".crapport"
#define DEFAULT_CRAPPORT_COLUMNS "benchmark run_config input_size exit_status runtime date ID"
#define DEFAULT_JULE_FILE_NAME   "crapport.j"
#define BUFFER_NAME              "*crapport"



typedef const char* Str;

typedef struct {
    union {
        Str       string;
        double    number;
        int       boolean;
    };
    int type;
} Value;

enum {
    STRING,
    NUMBER,
    BOOLEAN,
};



static uint64_t str_hash(Str s) {
    unsigned long hash = 5381;
    int c;

    while ((c = *s++))
    hash = ((hash << 5) + hash) + c; /* hash * 33 + c */

    return hash;
}

static int str_equ(Str a, Str b) { return strcmp(a, b) == 0; }
use_hash_table(Str, Value);
typedef hash_table(Str, Value) Value_Table;

typedef struct {
    Value_Table props;
} Experiment;


static array_t           experiments;
static pthread_mutex_t   experiments_lock = PTHREAD_MUTEX_INITIALIZER;
static Experiment        layout;
static tp_t             *tp;
static int               loading;
static yed_syntax        syn;
static int               jule_dirty;
static u64               jule_dirty_time_ms;
static int               jule_running;
static int               jule_finished;
static array_t           jule_output_chars;
static u64               jule_start_time_ms;

static void init_exp(Experiment *exp) {
    exp->props = hash_table_make_e(Str, Value, str_hash, str_equ);
}

static void free_exp(Experiment *exp) {
    Str    key;
    Value *val;

    if (exp->props != NULL) {
        hash_table_traverse(exp->props, key, val) {
            if (val->type == STRING) {
                free((char*)val->string);
            }
        }
        hash_table_free(exp->props);
    }
}

static void free_all(void) {
    Str         key;
    Experiment *exp;

    DBG("tearing down existing tables and threads");

    pthread_mutex_lock(&experiments_lock);

    array_traverse(experiments, exp) {
        free_exp(exp);
    }
    array_free(experiments);

    if (tp != NULL) {
        tp_stop(tp, TP_IMMEDIATE);
        tp_free(tp);
    }

out_unlock:;
    tp = NULL;
    pthread_mutex_unlock(&experiments_lock);
}

static void unload(yed_plugin *self) {
    free_all();
    yed_free_buffer(yed_get_or_create_special_rdonly_buffer(BUFFER_NAME));
    yed_syntax_free(&syn);
}

static Str get_crapport_dir(void) {
    Str dir;

    if ((dir = yed_get_var("crapport-dir")) == NULL) {
        dir = DEFAULT_CRAPPORT_DIR;
    }

    return dir;
}

#define RM_NL(_buff)                                          \
do {                                                          \
    int _len = strlen(_buff);                                 \
    if ((_buff)[_len - 1] == '\n') { (_buff)[_len - 1] = 0; } \
} while (0)

static inline int is_falsey(Str str) {
    int         i;
    const char *falsey[] = { "false", "False", "FALSE", "no",  "No",  "NO",  "off", "Off", "OFF" };

    for (i = 0; i < sizeof(falsey) / sizeof(falsey[0]); i += 1) {
        if (strcmp(str, falsey[i]) == 0) {
            return 1;
        }
    }

    return 0;
}

static inline int is_truthy(Str str) {
    int         i;
    const char *truthy[] = { "true",  "True",  "TRUE",  "yes", "Yes", "YES", "on",  "On",  "ON"  };

    for (i = 0; i < sizeof(truthy) / sizeof(truthy[0]); i += 1) {
        if (strcmp(str, truthy[i]) == 0) {
            return 1;
        }
    }

    return 0;
}

static inline int parse_number(Str str, double *d) {
    const char *eos;
    char       *end;

    eos = str + strlen(str);
    *d  = strtod(str, &end);

    return end == eos;
}

static inline Value parse_value(Str str) {
    Value val;

    if (is_falsey(str)) {
        val.type    = BOOLEAN;
        val.boolean = 0;
        goto out;
    }

    if (is_truthy(str)) {
        val.type    = BOOLEAN;
        val.boolean = 1;
        goto out;
    }

    if (parse_number(str, &val.number)) {
        val.type = NUMBER;
        goto out;
    }

    val.type   = STRING;
    val.string = strdup(str);

out:;
    return val;
}

static void load_exp(Str path) {
    Experiment  exp;
    char        buff[1024];
    FILE       *f;
    int         len;
    char       *key;
    int         i;
    Value       val;

    init_exp(&exp);

    snprintf(buff, sizeof(buff), "%s/props", path);
    f = fopen(buff, "r");
    if (f != NULL) {
        while (fgets(buff, sizeof(buff), f)) {
            RM_NL(buff);
            key = strdup(buff);
            if (fgets(buff, sizeof(buff), f)) {
                RM_NL(buff);
                val = parse_value(buff);
                hash_table_insert(exp.props, key, val);
            } else {
                free(key);
            }
        }
        fclose(f);
    }

    pthread_mutex_lock(&experiments_lock);
    array_push(experiments, exp);
    pthread_mutex_unlock(&experiments_lock);
}

static void load_exp_thr(void *arg) {
    load_exp(arg);
    free(arg);
}

static void *load_monitor_thr(void *arg) {
    Experiment *it;
    int         i;
    Value       v;

    loading = 1;
    tp_wait(tp);

    pthread_mutex_lock(&experiments_lock);
    i      = 0;
    v.type = NUMBER;
    array_traverse(experiments, it) {
        v.number = (double)i;
        hash_table_insert(it->props, strdup("ID"), v);
        i += 1;
    }
    pthread_mutex_unlock(&experiments_lock);

    loading = 0;
    yed_force_update();

    return NULL;
}

u32 platform_get_num_hw_threads(void) {
    u32 nprocs;

    nprocs = 0;

#ifdef __APPLE__
    int    nm[2];
    size_t len;

    nm[0] = CTL_HW;
    nm[1] = HW_AVAILCPU;
    len   = 4;
    sysctl(nm, 2, &nprocs, &len, NULL, 0);

    if (nprocs < 1) {
        nm[1] = HW_NCPU;
        sysctl(nm, 2, &nprocs, &len, NULL, 0);
    }
#else
    nprocs = get_nprocs();
#endif

    ASSERT(nprocs > 0, "failed to get the number of hw threads");

    return nprocs;
}

static void crapport_load(int n_args, char **args) {
    pthread_t      monitor_pthread;
    Str            dname;
    DIR           *dir;
    struct dirent *ent;
    char           path[1024];
    yed_buffer    *buff;

    if (n_args != 0) {
        yed_cerr("expected 0 arguments, but got %d", n_args);
        return;
    }

    dname = get_crapport_dir();

    DBG("starting load for dir '%s'", dname);

    dir = opendir(dname);
    if (dir == NULL) {
        yed_cerr("%s: %s", dname, strerror(errno));
        errno = 0;
        goto out;
    }

    free_all();

    pthread_mutex_lock(&experiments_lock);

    DBG("creating experiment table");
    experiments = array_make(Experiment);
    DBG("spinning up threadpool");
    tp          = tp_make(MAX(1, platform_get_num_hw_threads() - 1));

    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.'
        ||  ent->d_type    != DT_DIR) {

             continue;
        }

        snprintf(path, sizeof(path), "%s/%s", dname, ent->d_name);
        tp_add_task(tp, load_exp_thr, strdup(path));
    }

    pthread_create(&monitor_pthread, NULL, load_monitor_thr, NULL);
    pthread_detach(monitor_pthread);

    pthread_mutex_unlock(&experiments_lock);

    closedir(dir);

    buff = yed_get_or_create_special_rdonly_buffer(BUFFER_NAME);
    buff->flags &= ~BUFF_RD_ONLY;
    yed_buff_clear_no_undo(buff);
    yed_buff_insert_string_no_undo(buff, "Loading...", 1, 1);
    buff->flags |= BUFF_RD_ONLY;

out:;
}

static void crapport_set_columns(int n_args, char **args) {
    array_t chars;
    char    spc;
    int     i;
    int     j;
    int     len;

    chars = array_make(char);

    spc = ' ';

    for (i = 0; i < n_args; i += 1) {
        len = strlen(args[i]);
        for (j = 0; j < len; j += 1) {
            array_push(chars, args[i][j]);
        }
        array_push(chars, spc);
    }

    yed_set_var("crapport-columns", array_data(chars));

    array_free(chars);
}

static int value_width(Value *val) {
    switch (val->type) {
        case STRING:
            return strlen(val->string);
        case BOOLEAN:
            return 3; /* YES or NO */
        case NUMBER:
            return snprintf(NULL, 0, "%g", val->number);
    }
    return 0;
}

static int void_strcmp(const void *a, const void *b) {
    return strcmp(*(const char**)a, *(const char**)b);
}

static array_t get_keys(void) {
    const char *cols;
    array_t     keys;

    if ((cols = yed_get_var("crapport-columns")) == NULL) {
        cols = DEFAULT_CRAPPORT_COLUMNS;
    }

    keys = sh_split(cols);

    return keys;
}

static Str sort_key;
static int sort_type;

static int value_cmp(const Value *a, const Value *b) {
    switch (sort_type) {
        case STRING:
            if (a->type == STRING && b->type == STRING) {
                return strcmp(a->string, b->string);
            }
            if (a->type != STRING) { return  1; }
            if (b->type != STRING) { return -1; }
            return 0;
        case NUMBER:
            if (a->type == NUMBER && b->type == NUMBER) {
                if (a->number <  b->number) { return -1; }
                if (a->number == b->number) { return  0; }
                return 1;
            }
            if (a->type != NUMBER) { return  1; }
            if (b->type != NUMBER) { return -1; }
            return 0;
        case BOOLEAN:
            if (a->type == BOOLEAN && b->type == BOOLEAN) {
                return a->boolean - b->boolean;
                if (a->number <  b->number) { return -1; }
                if (a->number == b->number) { return  0; }
                return 1;
            }
            if (a->type != BOOLEAN) { return  1; }
            if (b->type != BOOLEAN) { return -1; }
            return 0;
    }
    return 0;
}

static int experiment_cmp(const void *a, const void *b) {
    const Experiment *ae;
    const Experiment *be;
    Value            *av;
    Value            *bv;
    int               r;

    ae = a;
    be = b;

    av = hash_table_get_val(ae->props, sort_key);
    bv = hash_table_get_val(be->props, sort_key);

    if (av == NULL && bv == NULL) {
        if (ae <= be) { return -1; }
        return 1;
    }
    if (av == NULL) { return  1; }
    if (bv == NULL) { return -1; }

    r = value_cmp(av, bv);

    if (r == 0) {
        if (ae <= be) { return -1; }
        return 1;
    }

    return r;
}

static void update_buffer(void) {
    yed_buffer *buff;
    Experiment *it;
    Str         key;
    Value      *val;
    Value       new_val;
    Value      *lookup;
    array_t     keys;
    int         row;
    int         col;
    Str        *key_it;
    int         width;
    char       *lazy_bar = "";
    char        s[256];
    array_t     sorted_experiments;

    buff = yed_get_or_create_special_rdonly_buffer(BUFFER_NAME);

    buff->flags &= ~BUFF_RD_ONLY;

    yed_buff_clear_no_undo(buff);

    if (loading) {
        yed_buff_insert_string_no_undo(buff, "Loading...", 1, 1);
        goto out_reset_rdonly;
    }

    pthread_mutex_lock(&experiments_lock);

    free_exp(&layout);
    init_exp(&layout);

    array_traverse(experiments, it) {
        hash_table_traverse(it->props, key, val) {
            new_val.type   = NUMBER;
            new_val.number = MAX(strlen(key), value_width(val));

            if ((lookup = hash_table_get_val(layout.props, key)) == NULL) {
                hash_table_insert(layout.props, strdup(key), new_val);
            } else {
                lookup->number = MAX(lookup->number, new_val.number);
            }
        }
    }

    keys = get_keys();

    row = 1;
    col = 2;
    array_traverse(keys, key_it) {
        key   = *key_it;
        val   = hash_table_get_val(layout.props, *key_it);
        if (val == NULL) { continue; }

        width = (int)val->number;
        snprintf(s, sizeof(s), "%s%*s", lazy_bar, -width, key);
        yed_buff_insert_string_no_undo(buff, s, row, col);
        col += width + 3 * !!lazy_bar[0];
        lazy_bar = " │ ";
    }
    row += 1;
    col  = 2;

    sorted_experiments = array_make(Experiment);
    array_copy(sorted_experiments, experiments);

    array_rtraverse(keys, key_it) {
        sort_key  = *key_it;
        sort_type = -1;

        array_traverse(experiments, it) {
            val = hash_table_get_val(it->props, sort_key);
            if (val == NULL) { continue; }

            if (sort_type < 0) {
                sort_type = val->type;
            } else if (val->type != sort_type && val->type == STRING) {
                sort_type = STRING;
            }
        }

        if (sort_type < 0) { sort_type = STRING; }

        qsort(array_data(sorted_experiments),
              array_len(sorted_experiments),
              sorted_experiments.elem_size,
              experiment_cmp);
    }

    array_traverse(sorted_experiments, it) {
        lazy_bar = "";
        array_traverse(keys, key_it) {
            key = *key_it;
            val = hash_table_get_val(layout.props, key);

            if (val == NULL) { continue; }

            width = (int)val->number;
            val   = hash_table_get_val(it->props, key);

            if (val == NULL) {
                snprintf(s, sizeof(s), "%s%*s", lazy_bar, -width, "");
            } else {
                switch (val->type) {
                    case STRING:
                        snprintf(s, sizeof(s), "%s%*s", lazy_bar, -width, val->string);
                        break;
                    case BOOLEAN:
                        snprintf(s, sizeof(s), "%s%*s", lazy_bar, width, ((int)val->boolean) ? "YES" : "NO");
                        break;
                    case NUMBER:
                        snprintf(s, sizeof(s), "%s%*g", lazy_bar, width, val->number);
                        break;

                }
            }
            yed_buff_insert_string_no_undo(buff, s, row, col);
            col += width + 3 * !!lazy_bar[0];
            lazy_bar = " │ ";
        }

        row += 1;
        col  = 2;
    }

    array_free(sorted_experiments);

    free_string_array(keys);

out_unlock:;
    pthread_mutex_unlock(&experiments_lock);

out_reset_rdonly:;
    buff->flags |= BUFF_RD_ONLY;
}

#define JULE_MAX_OUTPUT_LEN (64000)

static void jule_output_cb(const char *s, int n_bytes) {
    int         len;
    array_t     old;
    const char *message;

    array_push_n(jule_output_chars, (char*)s, n_bytes);
    len = array_len(jule_output_chars);
    if (len > JULE_MAX_OUTPUT_LEN) {
        old               = jule_output_chars;
        jule_output_chars = array_make_with_cap(char, len - JULE_MAX_OUTPUT_LEN);
        message = "... (output truncated)\n\n";
        array_push_n(jule_output_chars, (char*)message, strlen(message));
        array_push_n(jule_output_chars, array_data(old) + len - JULE_MAX_OUTPUT_LEN, JULE_MAX_OUTPUT_LEN);
        array_free(old);
    }
}

static Jule_Status jule_eval_cb(Jule_Value *value) {
    u64         now;
    const char *message;

    (void)value;

    now = measure_time_now_ms();

    if (now - jule_start_time_ms > 2500) {
        message = "crapport: TIMEOUT\n";
        jule_output_cb(message, strlen(message));
        return JULE_ERR_EVAL_CANCELLED;
    }

    return JULE_SUCCESS;
}

static void jule_error_cb(Jule_Error_Info *info) {
    Jule_Status  status;
    char         buff[1024];
    char        *s;

    status = info->status;

    snprintf(buff, sizeof(buff), "Jule Error: %s\n", jule_error_string(status));
    jule_output_cb(buff, strlen(buff));
    switch (status) {
        case JULE_ERR_UNEXPECTED_EOS:
        case JULE_ERR_UNEXPECTED_TOK:
            snprintf(buff, sizeof(buff), "    LINE:   %d, COLUMN: %d\n", info->location.line, info->location.col);
            jule_output_cb(buff, strlen(buff));
            break;
        default:
            snprintf(buff, sizeof(buff), "    LINE:   %d\n", info->location.line);
            jule_output_cb(buff, strlen(buff));
            break;
    }
    switch (status) {
        case JULE_ERR_LOOKUP:
            snprintf(buff, sizeof(buff), "    SYMBOL: %s\n", info->sym);
            jule_output_cb(buff, strlen(buff));
            break;
        case JULE_ERR_ARITY:
            snprintf(buff, sizeof(buff), "    WANTED: %s%d\n", info->arity_at_least ? "at least " : "", info->wanted_arity);
            jule_output_cb(buff, strlen(buff));
            snprintf(buff, sizeof(buff), "    GOT:    %d\n", info->got_arity);
            jule_output_cb(buff, strlen(buff));
            break;
        case JULE_ERR_TYPE:
            snprintf(buff, sizeof(buff), "    WANTED: %s\n", jule_type_string(info->wanted_type));
            jule_output_cb(buff, strlen(buff));
            snprintf(buff, sizeof(buff), "    GOT:    %s\n", jule_type_string(info->got_type));
            jule_output_cb(buff, strlen(buff));
            break;
        case JULE_ERR_OBJECT_KEY_TYPE:
            snprintf(buff, sizeof(buff), "    WANTED: number or string\n");
            jule_output_cb(buff, strlen(buff));
            snprintf(buff, sizeof(buff), "    GOT:    %s\n", jule_type_string(info->got_type));
            jule_output_cb(buff, strlen(buff));
            break;
        case JULE_ERR_NOT_A_FN:
            snprintf(buff, sizeof(buff), "    GOT:    %s\n", jule_type_string(info->got_type));
            jule_output_cb(buff, strlen(buff));
            break;
        case JULE_ERR_BAD_INDEX:
            s = jule_to_string(info->bad_index, 0);
            snprintf(buff, sizeof(buff), "    INDEX:  %s\n", s);
            jule_output_cb(buff, strlen(buff));
            JULE_FREE(s);
            break;
        default:
            break;
    }

    jule_free_error_info(info);
}

static void on_jule_update(void) {
    jule_dirty         = 1;
    jule_dirty_time_ms = measure_time_now_ms();
}

static char *j_columns_str;

static Jule_Status j_columns(Jule_Interp *interp, Jule_Value *tree, Jule_Array values, Jule_Value **result) {
    Jule_Status  status;
    array_t      chars;
    Jule_Value  *v;
    Jule_Value  *ev;
    unsigned     i;
    char         c;

    chars = array_make(char);

    if (j_columns_str != NULL) {
        free(j_columns_str);
        j_columns_str = NULL;
    }

    FOR_EACH(&values, v) {
        status = jule_eval(interp, v, &ev);
        if (status != JULE_SUCCESS) {
            *result = NULL;
            goto out_free;
        }
        if (ev->type != JULE_STRING) {
            status = JULE_ERR_TYPE;
            jule_make_type_error(interp, ev->line, JULE_STRING, ev->type);
            *result = NULL;
            goto out_free;
        }
        for (i = 0; i < ev->string.len; i += 1) {
            c = ev->string.chars[i];
            array_push(chars, c);
        }
        c = ' ';
        array_push(chars, c);
    }

    array_zero_term(chars);

    j_columns_str = array_data(chars);

    *result = jule_nil_value();

    goto out;

out_free:;
    array_free(chars);

out:;
    return status;
}

static Jule_Status j_filter(Jule_Interp *interp, Jule_Value *tree, Jule_Array values, Jule_Value **result) {

    *result = jule_nil_value();
    return JULE_SUCCESS;
}

static void create_jule_builtins(Jule_Interp *interp) {
    Jule_Value *table;
    Jule_Value *row;
    Experiment *exp;
    Str         key;
    Value      *val;
    Jule_Value *kv;
    Jule_Value *vv;

    table = jule_list_value();

    pthread_mutex_lock(&experiments_lock);
    array_traverse(experiments, exp) {
        row = jule_object_value();

        hash_table_traverse(exp->props, key, val) {
            kv = jule_string_value(key, strlen(key));
            if (val->type == STRING) {
                vv = jule_string_value(val->string, strlen(val->string));
            } else if (val->type == NUMBER) {
                vv = jule_number_value(val->number);
            } else if (val->type == BOOLEAN) {
                vv = jule_number_value(val->boolean);
            } else {
                break;
            }
            jule_insert(row, kv, vv);
        }

        jule_push(&table->list, row);
    }
    pthread_mutex_unlock(&experiments_lock);

    jule_install_var(interp, "@TABLE",   table);
    jule_install_fn(interp,  "@columns", j_columns);
    jule_install_fn(interp,  "@filter",  j_filter);
}

static void *jule_thread(void *arg) {
    char        *code;
    Jule_Interp  interp;
    Jule_Status  status;

    code = arg;

    array_free(jule_output_chars);
    jule_output_chars = array_make_with_cap(char, JULE_MAX_OUTPUT_LEN);

    jule_init_interp(&interp);
    jule_set_error_callback(&interp,  jule_error_cb);
    jule_set_output_callback(&interp, jule_output_cb);
    jule_set_eval_callback(&interp,   jule_eval_cb);

    create_jule_builtins(&interp);

    u64 start = measure_time_now_ms();
    status = jule_parse(&interp, code, strlen(code));
    if (status == JULE_SUCCESS) {
        jule_interp(&interp);
    }
    u64 end = measure_time_now_ms();

    char buff[64];
    snprintf(buff, sizeof(buff), "took %llu ms", end - start);
    jule_output_cb(buff, strlen(buff));

    jule_free(&interp);

    free(code);

    jule_running  = 0;
    jule_finished = 1;

    yed_force_update();

    return NULL;
}

static void update_jule(void) {
    pthread_t   t;
    char       *name;
    yed_buffer *buff;
    char       *code;

    if (jule_running) { return; }

    if ((name = yed_get_var("crapport-jule-file")) != NULL) {
        if ((buff = yed_get_buffer(name)) != NULL) {
            DBG("starting Jule interpreter");

            code = yed_get_buffer_text(buff);

            buff = yed_get_or_create_special_rdonly_buffer("*crapport-jule-output");
            buff->flags &= ~BUFF_RD_ONLY;
            yed_buff_clear_no_undo(buff);
            buff->flags |= BUFF_RD_ONLY;

            jule_running       = 1;
            jule_finished      = 0;
            jule_start_time_ms = measure_time_now_ms();
            pthread_create(&t, NULL, jule_thread, code);
            pthread_detach(t);
            yed_force_update();
        }
    }

    jule_dirty = 0;
}

static void epump(yed_event *event) {
    u64         now;
    yed_buffer *b;

    if (jule_dirty) {
        now = measure_time_now_ms();
        if (now - jule_dirty_time_ms >= 500) {
            update_jule();
        }
    } else if (jule_finished) {
        b = yed_get_or_create_special_rdonly_buffer("*crapport-jule-output");

        b->flags &= ~BUFF_RD_ONLY;
        yed_buff_clear_no_undo(b);
        array_zero_term(jule_output_chars);
        yed_buff_insert_string_no_undo(b, array_data(jule_output_chars), 1, 1);
        array_clear(jule_output_chars);
        b->flags |= BUFF_RD_ONLY;

        if (j_columns_str != NULL) {
            yed_set_var("crapport-columns", j_columns_str);
        }

        jule_finished = 0;
    }

    if (loading) {
        update_buffer();
    } else if (tp != NULL) {
        /* Tear down the threadpool if it's done loading, but still waiting for tasks. */
        DBG("noticed we're done loading... tearing down threadpool");
        pthread_mutex_lock(&experiments_lock);
        tp_stop(tp, TP_IMMEDIATE);
        tp_free(tp);
        tp = NULL;
        DBG("%d experiments loaded", array_len(experiments));
        pthread_mutex_unlock(&experiments_lock);

        update_buffer();
    }
}

static void eclear(yed_event *event) {
    yed_attrs attrs;

    if (event->frame->buffer != yed_get_or_create_special_rdonly_buffer(BUFFER_NAME)) {
        return;
    }

    event->row_base_attr = event->row & 1
                            ? yed_active_style_get_active()
                            : yed_active_style_get_inactive();
}

static void eload(yed_event *event) {
    const char *jule;

    if (event->buffer == NULL) { return; }

    if ((jule = yed_get_var("crapport-jule-file")) && strcmp(event->buffer->name, jule) == 0) {
        update_jule();
    }
}

static void ewrite(yed_event *event) {
    const char *jule;

    if (event->buffer == NULL) { return; }

    if ((jule = yed_get_var("crapport-jule-file")) && strcmp(event->buffer->name, jule) == 0) {
        update_jule();
    }
}


#define _CHECK(x, r)                                                      \
do {                                                                      \
    if (x) {                                                              \
        LOG_FN_ENTER();                                                   \
        yed_log("[!] " __FILE__ ":%d regex error for '%s': %s", __LINE__, \
                r,                                                        \
                yed_syntax_get_regex_err(&syn));                          \
        LOG_EXIT();                                                       \
    }                                                                     \
} while (0)

#define SYN()          yed_syntax_start(&syn)
#define ENDSYN()       yed_syntax_end(&syn)
#define APUSH(s)       yed_syntax_attr_push(&syn, s)
#define APOP(s)        yed_syntax_attr_pop(&syn)
#define RANGE(r)       _CHECK(yed_syntax_range_start(&syn, r), r)
#define ONELINE()      yed_syntax_range_one_line(&syn)
#define SKIP(r)        _CHECK(yed_syntax_range_skip(&syn, r), r)
#define ENDRANGE(r)    _CHECK(yed_syntax_range_end(&syn, r), r)
#define REGEX(r)       _CHECK(yed_syntax_regex(&syn, r), r)
#define REGEXSUB(r, g) _CHECK(yed_syntax_regex_sub(&syn, r, g), r)
#define KWD(k)         yed_syntax_kwd(&syn, k)

#ifdef __APPLE__
#define WB "[[:>:]]"
#else
#define WB "\\b"
#endif

static void estyle(yed_event *event)   { yed_syntax_style_event(&syn, event);         }
static void ebuffdel(yed_event *event) { yed_syntax_buffer_delete_event(&syn, event); }

static void ebuffmod(yed_event *event) {
    const char *jule;

    if (event->buffer == NULL) { return; }

    if ((jule = yed_get_var("crapport-jule-file")) && strcmp(event->buffer->name, jule) == 0) {
        on_jule_update();
    }

    yed_syntax_buffer_mod_event(&syn, event);
}

static void eline(yed_event *event) {
    yed_frame  *frame;
    yed_line   *line;
    yed_attrs   attrs;
    int         col;
    yed_glyph  *g;

    frame = event->frame;

    if (!frame || frame->buffer == NULL) { return; }

    if (frame->buffer != yed_get_or_create_special_rdonly_buffer(BUFFER_NAME)) {
        return;
    }

    if (event->row == 1) {
        line  = yed_buff_get_line(frame->buffer, event->row);
        attrs = yed_parse_attrs("&code-keyword bold");

        for (col = 1; col <= line->visual_width; col += 1) {
            g = yed_line_col_to_glyph(line, col);
            if (g != NULL && G_IS_ASCII(*g) && !is_space(g->c)) {
                yed_eline_combine_col_attrs(event, col, &attrs);
            }
        }
    } else {
        yed_syntax_line_event(&syn, event);
    }
}

static void evar(yed_event *event) {
    if (strcmp(event->var_name, "crapport-columns") == 0) {
        update_buffer();
    }
}

static int complete_columns(char *string, yed_completion_results *results) {
    array_t  columns;
    Str      key;
    Value   *val;
    int      status;

    columns = array_make(char*);

    if (layout.props != NULL) {
        hash_table_traverse(layout.props, key, val) {
            array_push(columns, key);
        }
    }

    FN_BODY_FOR_COMPLETE_FROM_ARRAY(string,
                                    array_len(columns),
                                    (char**)array_data(columns),
                                    results,
                                    status);

    array_free(columns);

    return status;
}

int yed_plugin_boot(yed_plugin *self) {
    yed_event_handler pump_handler;
    yed_event_handler clear_handler;
    yed_event_handler style_handler;
    yed_event_handler buffdel_handler;
    yed_event_handler buffmod_handler;
    yed_event_handler line_handler;
    yed_event_handler var_handler;
    yed_event_handler load_handler;
    yed_event_handler write_handler;


    YED_PLUG_VERSION_CHECK();

    yed_plugin_set_unload_fn(self, unload);

    yed_plugin_set_command(self, "crapport-load",        crapport_load);
    yed_plugin_set_command(self, "crapport-set-columns", crapport_set_columns);

    yed_plugin_set_completion(self, "crapport-set-columns-compl-arg-0",  complete_columns);
    yed_plugin_set_completion(self, "crapport-set-columns-compl-arg-1",  complete_columns);
    yed_plugin_set_completion(self, "crapport-set-columns-compl-arg-2",  complete_columns);
    yed_plugin_set_completion(self, "crapport-set-columns-compl-arg-3",  complete_columns);
    yed_plugin_set_completion(self, "crapport-set-columns-compl-arg-4",  complete_columns);
    yed_plugin_set_completion(self, "crapport-set-columns-compl-arg-5",  complete_columns);
    yed_plugin_set_completion(self, "crapport-set-columns-compl-arg-6",  complete_columns);
    yed_plugin_set_completion(self, "crapport-set-columns-compl-arg-7",  complete_columns);
    yed_plugin_set_completion(self, "crapport-set-columns-compl-arg-8",  complete_columns);
    yed_plugin_set_completion(self, "crapport-set-columns-compl-arg-9",  complete_columns);
    yed_plugin_set_completion(self, "crapport-set-columns-compl-arg-10", complete_columns);
    yed_plugin_set_completion(self, "crapport-set-columns-compl-arg-11", complete_columns);
    yed_plugin_set_completion(self, "crapport-set-columns-compl-arg-12", complete_columns);
    yed_plugin_set_completion(self, "crapport-set-columns-compl-arg-13", complete_columns);
    yed_plugin_set_completion(self, "crapport-set-columns-compl-arg-14", complete_columns);
    yed_plugin_set_completion(self, "crapport-set-columns-compl-arg-15", complete_columns);
    yed_plugin_set_completion(self, "crapport-set-columns-compl-arg-16", complete_columns);
    yed_plugin_set_completion(self, "crapport-set-columns-compl-arg-17", complete_columns);
    yed_plugin_set_completion(self, "crapport-set-columns-compl-arg-18", complete_columns);
    yed_plugin_set_completion(self, "crapport-set-columns-compl-arg-19", complete_columns);

    pump_handler.kind = EVENT_PRE_PUMP;
    pump_handler.fn   = epump;
    yed_plugin_add_event_handler(self, pump_handler);

    clear_handler.kind = EVENT_ROW_PRE_CLEAR;
    clear_handler.fn   = eclear;
    yed_plugin_add_event_handler(self, clear_handler);

    style_handler.kind = EVENT_STYLE_CHANGE;
    style_handler.fn   = estyle;
    yed_plugin_add_event_handler(self, style_handler);

    buffdel_handler.kind = EVENT_BUFFER_PRE_DELETE;
    buffdel_handler.fn   = ebuffdel;
    yed_plugin_add_event_handler(self, buffdel_handler);

    buffmod_handler.kind = EVENT_BUFFER_POST_MOD;
    buffmod_handler.fn   = ebuffmod;
    yed_plugin_add_event_handler(self, buffmod_handler);

    line_handler.kind = EVENT_LINE_PRE_DRAW;
    line_handler.fn   = eline;
    yed_plugin_add_event_handler(self, line_handler);

    var_handler.kind = EVENT_VAR_POST_SET;
    var_handler.fn   = evar;
    yed_plugin_add_event_handler(self, var_handler);

    var_handler.kind = EVENT_VAR_POST_UNSET;
    var_handler.fn   = evar;
    yed_plugin_add_event_handler(self, var_handler);

    load_handler.kind = EVENT_BUFFER_POST_LOAD;
    load_handler.fn   = eload;
    yed_plugin_add_event_handler(self, load_handler);

    write_handler.kind = EVENT_BUFFER_POST_WRITE;
    write_handler.fn   = ewrite;
    yed_plugin_add_event_handler(self, write_handler);

    SYN();
        APUSH("&code-number");
            REGEXSUB("(^|[^[:alnum:]_])(-?([[:digit:]]+\\.[[:digit:]]*)|(([[:digit:]]*\\.[[:digit:]]+)))"WB, 2);
            REGEXSUB("(^|[^[:alnum:]_])(-?[[:digit:]]+)"WB, 2);
            REGEXSUB("(^|[^[:alnum:]_])(0[xX][0-9a-fA-F]+)"WB, 2);
        APOP();
        APUSH("&green");
            KWD("YES");
        APOP();
        APUSH("&red");
            KWD("NO");
        APOP();
    ENDSYN();

    if (yed_get_var("crapport-dir") == NULL) {
        yed_set_var("crapport-dir", DEFAULT_CRAPPORT_DIR);
    }
    if (yed_get_var("crapport-jule-file") == NULL) {
        yed_set_var("crapport-jule-file", DEFAULT_JULE_FILE_NAME);
    }
    if (yed_get_var("crapport-columns") == NULL) {
        yed_set_var("crapport-columns", DEFAULT_CRAPPORT_COLUMNS);
    }

    yed_set_var("crapport-debug-log", "yes");

    yed_get_or_create_special_rdonly_buffer(BUFFER_NAME);
    yed_get_or_create_special_rdonly_buffer("*crapport-jule-output");

    return 0;
}
