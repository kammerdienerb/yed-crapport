#define _GNU_SOURCE

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
#include <inttypes.h>

#include <yed/plugin.h>
#include <yed/syntax.h>
#undef DBG

#include "tge.h"
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

enum {
    PLOT_SCATTER = 0,
    PLOT_LINE,
    PLOT_BAR,
};

typedef struct {
    char    *title;
    double   xmin;
    double   xmax;
    double   ymin;
    double   ymax;
    int      xmarks;
    int      ymarks;
    array_t  groups;
    int      type;
    int      height;
    int      width;
    double   axis_pad;
    int      invert_labels;
    int      point_labels;
    u32      fg;
    u32      bg;
    int      appearx;
    int      appeary;
} Plot;

#define POINT_COLOR_NOT_SET (0xffffffff)

typedef struct {
    array_t  points;
    u32      color;
    double   size;
    char    *label;
    double   labelx;
    double   labely;
} Plot_Point_Group;

typedef struct {
    double x;
    double y;
} Plot_Point;

static yed_plugin        *Self;
static array_t            experiments;
static array_t            experiments_working;
static pthread_mutex_t    experiments_lock = PTHREAD_MUTEX_INITIALIZER;
static Experiment         layout;
static tp_t              *tp;
static int                loading;
static yed_syntax         syn;
static TGE_Game          *tge;
static Jule_Interp        interp;
static pthread_mutex_t    jule_lock = PTHREAD_MUTEX_INITIALIZER;
static int                jule_dirty;
static u64                jule_dirty_time_ms;
static int                jule_finished;
static array_t            jule_output_chars;
static u64                jule_start_time_ms;
static int                jule_abort;
static array_t            jule_table_ids;
static array_t            jule_plots;
static int                has_err;
static int                err_fixed;
static char               err_msg[1024];
static yed_direct_draw_t *err_dd;
static char               err_file[512];
static int                err_line;
static int                err_col;
static int                err_has_loc;

static void init_exp(Experiment *exp) {
    exp->props = hash_table_make_e(Str, Value, str_hash, str_equ);
}

static void free_exp(Experiment *exp) {
    Str    key;
    Value *val;

    (void)key;

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

    (void)key;

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

    tp = NULL;
    pthread_mutex_unlock(&experiments_lock);
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
    unsigned i;
    const char *falsey[] = { "false", "False", "FALSE", "no",  "No",  "NO",  "off", "Off", "OFF" };

    for (i = 0; i < sizeof(falsey) / sizeof(falsey[0]); i += 1) {
        if (strcmp(str, falsey[i]) == 0) {
            return 1;
        }
    }

    return 0;
}

static inline int is_truthy(Str str) {
    unsigned i;
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
    char       *key;
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

    (void)arg;

    loading = 1;
    tp_wait(tp);

    pthread_mutex_lock(&experiments_lock);

    array_clear(experiments_working);

    i      = 0;
    v.type = NUMBER;
    array_traverse(experiments, it) {
        v.number = (double)i;
        hash_table_insert(it->props, strdup("ID"), v);
        i += 1;

        array_push(experiments_working, *it);
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

    (void)args;

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
    experiments         = array_make(Experiment);
    experiments_working = array_make(Experiment);
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

static unsigned value_width(Value *val) {
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

    return value_cmp(av, bv);
}

/*
 * Hybrid exponential search/linear search merge sort with hybrid
 * natural/pairwise first pass.  Requires about .3% more comparisons
 * for random data than LSMS with pairwise first pass alone.
 * It works for objects as small as two bytes.
 */

#define NATURAL
#define THRESHOLD 16    /* Best choice for natural merge cut-off. */

/* #define NATURAL to get hybrid natural merge.
 * (The default is pairwise merging.)
 */

static void ms_setup(uint8_t *, uint8_t *, size_t, size_t, int (*)(), void *);
static void ms_insertionsort(uint8_t *, size_t, size_t, int (*)(), void *);

#define ISIZE sizeof(int)
#define PSIZE sizeof(uint8_t *)
#define ICOPY_LIST(src, dst, last)                \
    do                            \
    *(int*)dst = *(int*)src, src += ISIZE, dst += ISIZE;    \
    while(src < last)
#define ICOPY_ELT(src, dst, i)                    \
    do                            \
    *(int*) dst = *(int*) src, src += ISIZE, dst += ISIZE;    \
    while (i -= ISIZE)

#define CCOPY_LIST(src, dst, last)        \
    do                    \
        *dst++ = *src++;        \
    while (src < last)
#define CCOPY_ELT(src, dst, i)            \
    do                    \
        *dst++ = *src++;        \
    while (i -= 1)

/*
 * Find the next possible pointer head.  (Trickery for forcing an array
 * to do double duty as a linked list when objects do not align with word
 * boundaries.
 */
/* Assumption: PSIZE is a power of 2. */
#define EVAL(p) (uint8_t **)                        \
    ((uint8_t *)0 +                            \
        (((uint8_t *)p + PSIZE - 1 - (uint8_t *) 0) & ~(PSIZE - 1)))


static int  ms_merge_sort_r(void *base, size_t nmemb, size_t size, int (*cmp)(const void *, const void *, void *), void *z);
static void ms_insertionsort(uint8_t *a, size_t n, size_t size, int (*cmp)(const void *, const void *, void *), void *z);
static void ms_setup(uint8_t *list1, uint8_t *list2, size_t n, size_t size, int (*cmp)(const void *, const void *, void *), void *z);

/**
 * Sorts array.
 *
 * @param vbase is base of array
 * @param nmemb is item count
 * @param size is item width
 * @param cmp is a callback returning <0, 0, or >0
 * @see mergesort_r()
 * @see heapsort()
 * @see qsort()
 */
static int merge_sort(void *base, size_t nmemb, size_t size, int (*cmp)(const void *, const void *)) {
    return ms_merge_sort_r(base, nmemb, size, (void *)cmp, 0);
}

/**
 * Sorts array w/ optional callback argument.
 *
 * @param base is base of array
 * @param nmemb is item count
 * @param size is item width
 * @param cmp is a callback returning <0, 0, or >0
 * @param z will optionally be passed as the third argument to cmp
 * @see mergesort()
 */
static int ms_merge_sort_r(void *base, size_t nmemb, size_t size, int (*cmp)(const void *, const void *, void *), void *z) {
    int i, sense;
    int big, iflag;
    uint8_t *f1, *f2, *t, *b, *tp2, *q, *l1, *l2;
    uint8_t *list2, *list1, *p2, *p, *last, **p1;

    if (size < PSIZE / 2)        /* Pointers must fit into 2 * size. */
    {
        errno = EINVAL;
        return -1;
    }

    if (nmemb == 0)
        return (0);

    /*
     * XXX
     * Stupid subtraction for the Cray.
     */
    iflag = 0;
    if (!(size % ISIZE) && !(((char *)base - (char *)0) % ISIZE))
        iflag = 1;

    if ((list2 = malloc(nmemb * size + PSIZE)) == NULL)
        return (-1);

    list1 = base;
    ms_setup(list1, list2, nmemb, size, cmp, z);
    last = list2 + nmemb * size;
    i = big = 0;
    while (*EVAL(list2) != last) {
        l2 = list1;
        p1 = EVAL(list1);
        for (tp2 = p2 = list2; p2 != last; p1 = EVAL(l2)) {
            p2 = *EVAL(p2);
            f1 = l2;
            f2 = l1 = list1 + (p2 - list2);
            if (p2 != last)
                p2 = *EVAL(p2);
            l2 = list1 + (p2 - list2);
            while (f1 < l1 && f2 < l2) {
                if ((*cmp)(f1, f2, z) <= 0) {
                    q = f2;
                    b = f1, t = l1;
                    sense = -1;
                } else {
                    q = f1;
                    b = f2, t = l2;
                    sense = 0;
                }
                if (!big) {    /* here i = 0 */
                    while ((b += size) < t && cmp(q, b, z) >sense)
                        if (++i == 6) {
                            big = 1;
                            goto EXPONENTIAL;
                        }
                } else {
EXPONENTIAL:                for (i = size; ; i <<= 1)
                        if ((p = (b + i)) >= t) {
                            if ((p = t - size) > b &&
                            (*cmp)(q, p, z) <= sense)
                                t = p;
                            else
                                b = p;
                            break;
                        } else if ((*cmp)(q, p, z) <= sense) {
                            t = p;
                            if (i == (int)size)
                                big = 0;
                            goto FASTCASE;
                        } else
                            b = p;
                    while (t > b+size) {
                        i = (((t - b) / size) >> 1) * size;
                        if ((*cmp)(q, p = b + i, z) <= sense)
                            t = p;
                        else
                            b = p;
                    }
                    goto COPY;
FASTCASE:                while (i > (int)size)
                        if ((*cmp)(q, p = b + (i >>= 1), z)
                        <= sense)
                            t = p;
                        else
                            b = p;
COPY:                    b = t;
                }
                i = size;
                if (q == f1) {
                    if (iflag) {
                        ICOPY_LIST(f2, tp2, b);
                        ICOPY_ELT(f1, tp2, i);
                    } else {
                        CCOPY_LIST(f2, tp2, b);
                        CCOPY_ELT(f1, tp2, i);
                    }
                } else {
                    if (iflag) {
                        ICOPY_LIST(f1, tp2, b);
                        ICOPY_ELT(f2, tp2, i);
                    } else {
                        CCOPY_LIST(f1, tp2, b);
                        CCOPY_ELT(f2, tp2, i);
                    }
                }
            }
            if (f2 < l2) {
                if (iflag)
                    ICOPY_LIST(f2, tp2, l2);
                else
                    CCOPY_LIST(f2, tp2, l2);
            } else if (f1 < l1) {
                if (iflag)
                    ICOPY_LIST(f1, tp2, l1);
                else
                    CCOPY_LIST(f1, tp2, l1);
            }
            *p1 = l2;
        }
        tp2 = list1;    /* swap list1, list2 */
        list1 = list2;
        list2 = tp2;
        last = list2 + nmemb*size;
    }
    if (base == list2) {
        memmove(list2, list1, nmemb*size);
        list2 = list1;
    }
    free(list2);
    return (0);
}

#define    swap(a, b) {                    \
        s = b;                    \
        i = size;                \
        do {                    \
            tmp = *a; *a++ = *s; *s++ = tmp; \
        } while (--i);                \
        a -= size;                \
    }
#define reverse(bot, top) {                \
    s = top;                    \
    do {                        \
        i = size;                \
        do {                    \
            tmp = *bot; *bot++ = *s; *s++ = tmp; \
        } while (--i);                \
        s -= size2;                \
    } while(bot < s);                \
}

/*
 * Optional hybrid natural/pairwise first pass.  Eats up list1 in runs of
 * increasing order, list2 in a corresponding linked list.  Checks for runs
 * when THRESHOLD/2 pairs compare with same sense.  (Only used when NATURAL
 * is defined.  Otherwise simple pairwise merging is used.)
 */
static void ms_setup(uint8_t *list1, uint8_t *list2, size_t n, size_t size, int (*cmp)(const void *, const void *, void *), void *z) {
    int i, length, size2, sense;
    uint8_t tmp, *f1, *f2, *s, *l2, *last, *p2;

    size2 = size*2;
    if (n <= 5) {
        ms_insertionsort(list1, n, size, cmp, z);
        *EVAL(list2) = (uint8_t*) list2 + n*size;
        return;
    }
    /*
     * Avoid running pointers out of bounds; limit n to evens
     * for simplicity.
     */
    i = 4 + (n & 1);
    ms_insertionsort(list1 + (n - i) * size, i, size, cmp, z);
    last = list1 + size * (n - i);
    *EVAL(list2 + (last - list1)) = list2 + n * size;

#ifdef NATURAL
    p2 = list2;
    f1 = list1;
    sense = (cmp(f1, f1 + size, z) > 0);
    for (; f1 < last; sense = !sense) {
        length = 2;
                    /* Find pairs with same sense. */
        for (f2 = f1 + size2; f2 < last; f2 += size2) {
            if ((cmp(f2, f2+ size, z) > 0) != sense)
                break;
            length += 2;
        }
        if (length < THRESHOLD) {        /* Pairwise merge */
            do {
                p2 = *EVAL(p2) = f1 + size2 - list1 + list2;
                if (sense > 0)
                    swap (f1, f1 + size);
            } while ((f1 += size2) < f2);
        } else {                /* Natural merge */
            l2 = f2;
            for (f2 = f1 + size2; f2 < l2; f2 += size2) {
                if ((cmp(f2-size, f2, z) > 0) != sense) {
                    p2 = *EVAL(p2) = f2 - list1 + list2;
                    if (sense > 0)
                        reverse(f1, f2-size);
                    f1 = f2;
                }
            }
            if (sense > 0)
                reverse (f1, f2-size);
            f1 = f2;
            if (f2 < last || cmp(f2 - size, f2, z) > 0)
                p2 = *EVAL(p2) = f2 - list1 + list2;
            else
                p2 = *EVAL(p2) = list2 + n*size;
        }
    }
#else        /* pairwise merge only. */
    for (f1 = list1, p2 = list2; f1 < last; f1 += size2) {
        p2 = *EVAL(p2) = p2 + size2;
        if (cmp (f1, f1 + size, z) > 0)
            swap(f1, f1 + size);
    }
#endif /* NATURAL */
}

/*
 * This is to avoid out-of-bounds addresses in sorting the
 * last 4 elements.
 */
static void ms_insertionsort(uint8_t *a, size_t n, size_t size, int (*cmp)(const void *, const void *, void *), void *z) {
    uint8_t *ai, *s, *t, *u, tmp;
    int i;

    for (ai = a+size; --n >= 1; ai += size)
        for (t = ai; t > a; t -= size) {
            u = t - size;
            if (cmp(u, t, z) <= 0)
                break;
            swap(u, t);
        }
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

    array_traverse(experiments_working, it) {
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
    array_copy(sorted_experiments, experiments_working);

    array_rtraverse(keys, key_it) {
        sort_key  = *key_it;
        sort_type = -1;

        array_traverse(experiments_working, it) {
            val = hash_table_get_val(it->props, sort_key);
            if (val == NULL) { continue; }

            if (sort_type < 0) {
                sort_type = val->type;
            } else if (val->type != sort_type && val->type == STRING) {
                sort_type = STRING;
            }
        }

        if (sort_type < 0) { sort_type = STRING; }

        merge_sort(array_data(sorted_experiments),
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

    pthread_mutex_unlock(&experiments_lock);

out_reset_rdonly:;
    buff->flags |= BUFF_RD_ONLY;
}

#define JULE_MAX_OUTPUT_LEN (64000)

FILE *f;

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
    const char *message;

    (void)value;

    if (unlikely(jule_abort)) {
        message = "crapport: TIMEOUT\n";
        jule_output_cb(message, strlen(message));
        return JULE_ERR_EVAL_CANCELLED;
    }

    return JULE_SUCCESS;
}


static void set_err(int has_loc, char *file, int line, int col, const char *msg) {
    int max_err_len;

    has_err = 1;

    err_has_loc = has_loc;
    err_file[0] = 0;
    if (err_has_loc) {
        strcat(err_file, file);
        err_line    = line;
        err_col     = MAX(col, 1);
    }
    err_fixed   = 0;
    err_msg[0]  = 0;

    max_err_len = (ys->term_cols / 4 * 3) - 4; /* 4 is the padding on both sides. */

    strncat(err_msg, msg, max_err_len - 3);
    if ((int)strlen(msg) > max_err_len) {
        strcat(err_msg, "...");
    }
}

static void jule_error_cb(Jule_Error_Info *info) {
    Jule_Status  status;
    char         buff[1024];
    char         dd_buff[4096];
    char        *s;

    status = info->status;

    snprintf(buff, sizeof(buff), "%s:%u:%u: error: %s",
             info->file == NULL ? "<?>" : info->file,
             info->location.line,
             info->location.col,
             jule_error_string(status));
    jule_output_cb(buff, strlen(buff));

    dd_buff[0] = 0;
    strcat(dd_buff, buff);

    switch (status) {
        case JULE_ERR_LOOKUP:
        case JULE_ERR_RELEASE_WHILE_BORROWED:
            snprintf(buff, sizeof(buff), " (%s)", info->sym);
            jule_output_cb(buff, strlen(buff));
            break;
        case JULE_ERR_ARITY:
            snprintf(buff, sizeof(buff), " (wanted %s%d, got %d)",
                    info->arity_at_least ? "at least " : "",
                    info->wanted_arity,
                    info->got_arity);
            jule_output_cb(buff, strlen(buff));
            break;
        case JULE_ERR_TYPE:
            snprintf(buff, sizeof(buff), " (wanted %s, got %s)",
                    jule_type_string(info->wanted_type),
                    jule_type_string(info->got_type));
            jule_output_cb(buff, strlen(buff));
            break;
        case JULE_ERR_OBJECT_KEY_TYPE:
            snprintf(buff, sizeof(buff), " (wanted number or string, got %s)", jule_type_string(info->got_type));
            jule_output_cb(buff, strlen(buff));
            break;
        case JULE_ERR_NOT_A_FN:
            snprintf(buff, sizeof(buff), " (got %s)", jule_type_string(info->got_type));
            jule_output_cb(buff, strlen(buff));
            break;
        case JULE_ERR_BAD_INDEX:
            s = jule_to_string(info->interp, info->bad_index, 0);
            snprintf(buff, sizeof(buff), " (index: %s)", s);
            jule_output_cb(buff, strlen(buff));
            JULE_FREE(s);
            break;
        case JULE_ERR_FILE_NOT_FOUND:
        case JULE_ERR_FILE_IS_DIR:
        case JULE_ERR_MMAP_FAILED:
            snprintf(buff, sizeof(buff), " (%s)", info->path);
            jule_output_cb(buff, strlen(buff));
            break;
        default:
            break;
    }

    strcat(dd_buff, buff);

    set_err(info->file != NULL,
            info->file,
            info->location.line,
            info->location.col,
            dd_buff);

    snprintf(buff, sizeof(buff), "\n");
    jule_output_cb(buff, strlen(buff));

    jule_free_error_info(info);
}

static void on_jule_update(void) {
    jule_dirty         = 1;
    jule_dirty_time_ms = measure_time_now_ms();
}

static char *j_columns_str;

static Jule_Status j_display_columns(Jule_Interp *interp, Jule_Value *tree, unsigned n_values, Jule_Value **values, Jule_Value **result) {
    Jule_Status        status;
    array_t            chars;
    unsigned           i;
    Jule_Value        *v;
    Jule_Value        *ev;
    const Jule_String *string;
    unsigned           j;
    char               c;

    (void)interp;
    (void)tree;

    chars = array_make(char);

    if (j_columns_str != NULL) {
        free(j_columns_str);
        j_columns_str = NULL;
    }

    for (i = 0; i < n_values; i += 1) {
        v      = values[i];
        status = jule_eval(interp, v, &ev);
        if (status != JULE_SUCCESS) {
            *result = NULL;
            goto out_free;
        }
        if (ev->type != JULE_STRING) {
            status = JULE_ERR_TYPE;
            jule_make_type_error(interp, ev, JULE_STRING, ev->type);
            *result = NULL;
            goto out_free;
        }
        string = jule_get_string(interp, ev->string_id);
        for (j = 0; j < string->len; j += 1) {
            c = string->chars[j];
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

static Jule_Status j_filter(Jule_Interp *interp, Jule_Value *tree, unsigned n_values, Jule_Value **values, Jule_Value **result) {
    Jule_Status          status;
    Jule_Value          *expr;
    Jule_String_ID       table_id;
    Jule_String_ID       row_id;
    Jule_Value          *table;
    array_t              delete_rows;
    unsigned long long   idx;
    Jule_Value          *row;
    Jule_Value          *expr_result;
    int                  keep_row;
    Jule_Value         **it;

    status = jule_args(interp, tree, "-*", n_values, values, &expr);
    if (status != JULE_SUCCESS) {
        *result = NULL;
        goto out;
    }

    table_id = jule_get_string_id(interp, "@table");
    row_id   = jule_get_string_id(interp, "@row");

    table = jule_lookup(interp, table_id);
    if (table == NULL) {
        status = JULE_ERR_LOOKUP;
        jule_make_lookup_error(interp, tree, table_id);
        *result = NULL;
        goto out;
    }

    if (table->type != JULE_LIST) { goto out; }

    delete_rows = array_make(Jule_Value*);

    JULE_BORROW(table);

    idx = 0;
    FOR_EACH(table->list, row) {
        JULE_BORROWER(row);

        row->in_symtab = 1;

        jule_install_var(interp, row_id, row);
        status = jule_eval(interp, expr, &expr_result);
        if (status != JULE_SUCCESS) {
            JULE_UNBORROWER(row);
            jule_uninstall_var_no_free(interp, row_id);
            *result = NULL;
            goto out_unborrow;
        }
        if (expr_result->type != JULE_NUMBER) {
            status = JULE_ERR_TYPE;
            JULE_UNBORROWER(row);
            jule_uninstall_var_no_free(interp, row_id);
            jule_make_type_error(interp, expr, JULE_NUMBER, expr_result->type);
            *result = NULL;
            goto out_unborrow;
        }
        keep_row = expr_result->number != 0;
        jule_free_value(expr_result);
        JULE_UNBORROWER(row);
        jule_uninstall_var_no_free(interp, row_id);

        if (!keep_row) {
            array_push(delete_rows, table->list->data[idx]);
        }

        idx += 1;
    }

    array_traverse(delete_rows, it) {
again:;
        idx = 0;
        FOR_EACH(table->list, row) {
            if (row == *it) {
                jule_free_value_force(row);
                jule_erase(table->list, idx);
                goto again;
            }
            idx += 1;
        }
    }

    *result = table;

out_unborrow:;
    JULE_UNBORROW(table);

    array_free(delete_rows);

out:;
    return status;
}

static Jule_Status j_plot(Jule_Interp *interp, Jule_Value *tree, unsigned n_values, Jule_Value **values, Jule_Value **result) {
    Jule_Status        status;
    Jule_Value        *object;
    Plot               plot;
    const Jule_String *string;
    Jule_Value        *title;
    Jule_Value        *type;
    Jule_Value        *fg;
    Jule_Value        *bg;
    Jule_Value        *invert_labels;
    Jule_Value        *point_labels;
    Jule_Value        *width;
    Jule_Value        *height;
    Jule_Value        *xmin;
    Jule_Value        *xmax;
    Jule_Value        *ymin;
    Jule_Value        *ymax;
    Jule_Value        *xmarks;
    Jule_Value        *ymarks;
    Jule_Value        *appearx;
    Jule_Value        *appeary;
    Jule_Value        *groups;
    Jule_Value        *group;
    Plot_Point_Group   point_group;
    Plot_Point         plot_point;
    Jule_Value        *size;
    Jule_Value        *label;
    Jule_Value        *labelx;
    Jule_Value        *labely;
    Jule_Value        *color;
    char              *s;
    Jule_Value        *points;
    Jule_Value        *point;
    Jule_Value        *x;
    Jule_Value        *y;

    status = jule_args(interp, tree, "o", n_values, values, &object);
    if (status != JULE_SUCCESS) {
        *result = NULL;
        goto out;
    }

    memset(&plot, 0, sizeof(plot));

    plot.height   = 64;
    plot.width    = 64;
    plot.axis_pad = 0.15;

#define GET_FIELD(_name, _object, _field, _type)                  \
do {                                                              \
    Jule_Value *_key_value = jule_string_value(interp, (_field)); \
    (_name) = jule_field((_object), _key_value);                  \
    if ((_name) != NULL) {                                        \
        if ((_name)->type != (_type)) { (_name) = NULL; }         \
    }                                                             \
    jule_free_value(_key_value);                                  \
} while (0)

    GET_FIELD(title, object, "title", JULE_STRING);
    plot.title = title == NULL
                    ? strdup("Untitled Plot")
                    : jule_to_string(interp, title, JULE_NO_QUOTE);

    GET_FIELD(type, object, "type", JULE_STRING);
    if (type != NULL) {
        string = jule_get_string(interp, type->string_id);
             if (string->len == strlen("scatter") && strncmp(string->chars, "scatter", strlen("scatter")) == 0)
                { plot.type = PLOT_SCATTER; }
        else if (string->len == strlen("line")    && strncmp(string->chars, "line",    strlen("line"))    == 0)
                { plot.type = PLOT_LINE; }
        else if (string->len == strlen("bar")     && strncmp(string->chars, "bar",     strlen("bar"))     == 0)
                { plot.type = PLOT_BAR; }
    }

    GET_FIELD(fg,            object, "fg",            JULE_STRING);
    GET_FIELD(bg,            object, "bg",            JULE_STRING);
    GET_FIELD(invert_labels, object, "invert-labels", JULE_NUMBER);
    GET_FIELD(point_labels,  object, "point-labels",  JULE_NUMBER);
    GET_FIELD(width,         object, "width",         JULE_NUMBER);
    GET_FIELD(height,        object, "height",        JULE_NUMBER);
    GET_FIELD(appearx,       object, "appearx",       JULE_NUMBER);
    GET_FIELD(appeary,       object, "appeary",       JULE_NUMBER);
    GET_FIELD(xmin,          object, "xmin",          JULE_NUMBER);
    GET_FIELD(xmax,          object, "xmax",          JULE_NUMBER);
    GET_FIELD(ymin,          object, "ymin",          JULE_NUMBER);
    GET_FIELD(ymax,          object, "ymax",          JULE_NUMBER);
    GET_FIELD(xmarks,        object, "xmarks",        JULE_NUMBER);
    GET_FIELD(ymarks,        object, "ymarks",        JULE_NUMBER);

    plot.fg = 0;
    if (fg != NULL && (string = jule_get_string(interp, fg->string_id)) && string->len < 32) {
        s = alloca(string->len + 1);
        memcpy(s, string->chars, string->len);
        s[string->len] = 0;
        sscanf(s + (s[0] == '#'), "%x", &plot.fg);
    }

    plot.bg = 0xeeeeee;
    if (bg != NULL && (string = jule_get_string(interp, bg->string_id)) && string->len < 32) {
        s = alloca(string->len + 1);
        memcpy(s, string->chars, string->len);
        s[string->len] = 0;
        sscanf(s + (s[0] == '#'), "%x", &plot.bg);
    }

    plot.invert_labels = invert_labels != NULL && invert_labels->number != 0;
    plot.point_labels  = point_labels  != NULL && point_labels->number  != 0;

    plot.width  = width  == NULL ? 0 : width->number;
    plot.height = height == NULL ? 0 : height->number;
    if (plot.width  <= 0 || plot.width  > 256) { plot.width  = 64; }
    if (plot.height <= 0 || plot.height > 256) { plot.height = 64; }

    plot.appearx = appearx == NULL ? 0 : (int)(appearx->number * ys->term_cols);
    plot.appeary = appeary == NULL ? 0 : (int)(appeary->number * ys->term_rows);

    plot.xmin   = xmin   == NULL ? 0  : xmin->number;
    plot.xmax   = xmax   == NULL ? 1  : xmax->number;
    plot.ymin   = ymin   == NULL ? 0  : ymin->number;
    plot.ymax   = ymax   == NULL ? 1  : ymax->number;
    plot.xmarks = xmarks == NULL ? 0  : (int)xmarks->number;
    plot.ymarks = ymarks == NULL ? 10 : (int)ymarks->number;

    plot.groups = array_make(Plot_Point_Group);

    GET_FIELD(groups, object, "groups", JULE_LIST);
    if (groups != NULL) {
        FOR_EACH(groups->list, group) {
            if (group->type != JULE_OBJECT) { continue; }
            memset(&point_group, 0, sizeof(point_group));

            point_group.points = array_make(Plot_Point);

            GET_FIELD(size,   group, "size",   JULE_NUMBER);
            GET_FIELD(label,  group, "label",  JULE_STRING);
            GET_FIELD(labelx, group, "labelx", JULE_NUMBER);
            GET_FIELD(labely, group, "labely", JULE_NUMBER);
            GET_FIELD(color,  group, "color",  JULE_STRING);

            point_group.size   = size   == NULL ? 0    : size->number;
            point_group.label  = label  == NULL ? NULL : jule_to_string(interp, label, JULE_NO_QUOTE);
            point_group.labelx = labelx == NULL ? NAN  : labelx->number;
            point_group.labely = labely == NULL ? NAN  : labely->number;

            point_group.color = POINT_COLOR_NOT_SET;
            if (color != NULL && (string = jule_get_string(interp, color->string_id)) && string->len < 32) {
                s = alloca(string->len + 1);
                memcpy(s, string->chars, string->len);
                s[string->len] = 0;
                sscanf(s + (s[0] == '#'), "%x", &point_group.color);
            }

            GET_FIELD(points, group, "points", JULE_LIST);
            if (points != NULL) {
                FOR_EACH(points->list, point) {
                    if (point->type != JULE_OBJECT) { continue; }
                    memset(&plot_point, 0, sizeof(plot_point));

                    GET_FIELD(x, point, "x", JULE_NUMBER);
                    GET_FIELD(y, point, "y", JULE_NUMBER);

                    plot_point.x = x == NULL ? 0 : x->number;
                    plot_point.y = y == NULL ? 0 : y->number;

                    plot.xmin = xmin == NULL ? MIN(plot.xmin, plot_point.x) : plot.xmin;
                    plot.xmax = xmax == NULL ? MAX(plot.xmax, plot_point.x) : plot.xmax;
                    plot.ymin = ymin == NULL ? MIN(plot.ymin, plot_point.y) : plot.ymin;
                    plot.ymax = ymax == NULL ? MAX(plot.ymax, plot_point.y) : plot.ymax;

                    array_push(point_group.points, plot_point);
                }
            }

            array_push(plot.groups, point_group);
        }
    }

    array_push(jule_plots, plot);

    *result = jule_nil_value();

out:;
    return status;
}

static Jule_Status j_color(Jule_Interp *interp, Jule_Value *tree, unsigned n_values, Jule_Value **values, Jule_Value **result) {
    Jule_Status        status;
    Jule_Value        *s;
    const Jule_String *string;
    char               buff[64];
    yed_attrs          attrs;

    status = jule_args(interp, tree, "s", n_values, values, &s);
    if (status != JULE_SUCCESS) {
        *result = NULL;
        goto out;
    }

    string = jule_get_string(interp, s->string_id);

    snprintf(buff, sizeof(buff), "%.*s", (int)string->len, string->chars);

    /* @bad I think this is totally not okay to do from this thread... */
    attrs = yed_parse_attrs(buff);


    snprintf(buff, sizeof(buff), "#%06x", attrs.fg);

    *result = jule_string_value(interp, buff);

out:;
    return status;

}

static void create_jule_builtins(Jule_Interp *interp) {
    Jule_Value *table;
    Experiment *exp;
    Jule_Value *row;
    Str         key;
    Value      *val;
    Value      *lookup;
    Jule_Value *kv;
    Jule_Value *vv;
    Jule_Value *columns;

    table = jule_list_value();

    pthread_mutex_lock(&experiments_lock);
    array_traverse(experiments, exp) {
        row = jule_object_value();

        if (layout.props != NULL) {
            hash_table_traverse(layout.props, key, val) {
                kv = jule_string_value(interp, key);

                lookup = hash_table_get_val(exp->props, key);

                if (lookup == NULL) {
                    jule_insert(row, kv, jule_nil_value());
                } else {
                    val = lookup;

                    if (val->type == STRING) {
                        vv = jule_string_value(interp, val->string);
                    } else if (val->type == NUMBER) {
                        vv = jule_number_value(val->number);
                    } else if (val->type == BOOLEAN) {
                        vv = jule_number_value(val->boolean);
                    } else {
                        break;
                    }
                    jule_insert(row, kv, vv);
                }

            }
        }

        table->list = jule_push(table->list, row);
    }

    columns = jule_list_value();
    if (layout.props != NULL) {
        hash_table_traverse(layout.props, key, val) {
            kv = jule_string_value(interp, key);
            columns->list = jule_push(columns->list, kv);
        }
    }

    pthread_mutex_unlock(&experiments_lock);


    jule_install_var(interp, jule_get_string_id(interp, "@table"),           table);
    jule_install_var(interp, jule_get_string_id(interp, "@columns"),         columns);
    jule_install_fn(interp,  jule_get_string_id(interp, "@display-columns"), j_display_columns);
    jule_install_fn(interp,  jule_get_string_id(interp, "@filter"),          j_filter);
    jule_install_fn(interp,  jule_get_string_id(interp, "@plot"),            j_plot);
    jule_install_fn(interp,  jule_get_string_id(interp, "@color"),           j_color);
}

static void *jule_timeout_thread(void *arg) {
    u64 now;

    (void)arg;

    for (;;) {
        if (jule_finished) { break; }

        if (pthread_mutex_trylock(&jule_lock) == 0) {
            pthread_mutex_unlock(&jule_lock);
            break;
        }

        usleep(100000);

        now = measure_time_now_ms();

        if (0 && now - jule_start_time_ms > 2500) {
            jule_abort = 1;
            break;
        }
    }

    return NULL;
}

static char jule_file_buff[1024];

static void *jule_thread(void *arg) {
    char        *code;
    Jule_Status  status;

    code = arg;

    array_free(jule_output_chars);
    jule_output_chars = array_make_with_cap(char, JULE_MAX_OUTPUT_LEN);

    array_free(jule_table_ids);
    jule_table_ids = array_make(int);

    array_free(jule_plots);
    jule_plots = array_make(Plot);

    jule_init_interp(&interp);
    jule_set_error_callback(&interp,  jule_error_cb);
    jule_set_output_callback(&interp, jule_output_cb);
    jule_set_eval_callback(&interp,   jule_eval_cb);
    interp.cur_file = jule_get_string_id(&interp, jule_file_buff);

    create_jule_builtins(&interp);

    u64 start = measure_time_now_ms();
    status = jule_parse(&interp, code, strlen(code));
    if (status == JULE_SUCCESS) {
        jule_interp(&interp);
    }
    u64 end = measure_time_now_ms();

    char buff[64];
    snprintf(buff, sizeof(buff), "took %"PRIu64" ms", end - start);
    jule_output_cb(buff, strlen(buff));

    free(code);

    jule_finished = 1;

    yed_force_update();

    return NULL;
}

static yed_attrs get_err_attrs(void) {
    yed_attrs active;
    yed_attrs a;
    yed_attrs red;
    float     brightness;

    active = yed_active_style_get_active();

    a = active;

    if (ATTR_FG_KIND(a.flags) == ATTR_KIND_RGB) {
        red        = yed_active_style_get_red();
        brightness = ((RGB_32_r(active.bg) + RGB_32_g(active.bg) + RGB_32_b(active.bg)) / 3) / 255.0f;
        a.bg       = RGB_32(RGB_32_r(red.fg) / 2 + (u32)(brightness * 0x7f),
                            RGB_32_g(red.fg) / 2 + (u32)(brightness * 0x7f),
                            RGB_32_b(red.fg) / 2 + (u32)(brightness * 0x7f));
    } else {
        a = yed_parse_attrs("&active.bg &attention.fg swap");
    }

    return a;
}

static void draw_error_message(int do_draw) {
    char      line_buff[sizeof(err_msg) + 8];
    int       line_len;
    int       n_glyphs;
    int       line_width;

    if ((do_draw && err_dd) || (!do_draw && !err_dd)) {
        return;
    }

    if (do_draw) {
        if (err_has_loc) {
            sprintf(line_buff, "  %s  ", err_msg);
            line_len = strlen(line_buff);
            yed_get_string_info(line_buff, line_len, &n_glyphs, &line_width);

            err_dd = yed_direct_draw(ys->term_rows - 3,
                                     ys->term_cols - line_width,
                                     get_err_attrs(),
                                     line_buff);
        }
    } else {
        yed_kill_direct_draw(err_dd);
        err_dd = 0;
    }
}



static void update_jule(void) {
    pthread_t   t;
    char       *name;
    yed_buffer *buff;
    char       *code;

    if ((name = yed_get_var("crapport-jule-file")) != NULL) {
        if ((buff = yed_get_buffer(name)) != NULL) {

            if (pthread_mutex_trylock(&jule_lock) != 0) { return; }

            has_err   = 0;
            err_fixed = 1;

            draw_error_message(0);

            snprintf(jule_file_buff, sizeof(jule_file_buff), "%s", name);

            DBG("starting Jule interpreter");

            code = yed_get_buffer_text(buff);

            buff = yed_get_or_create_special_rdonly_buffer("*crapport-jule-output");
            buff->flags &= ~BUFF_RD_ONLY;
            yed_buff_clear_no_undo(buff);
            buff->flags |= BUFF_RD_ONLY;

            jule_finished      = 0;
            jule_abort         = 0;
            jule_start_time_ms = measure_time_now_ms();
            pthread_create(&t, NULL, jule_thread, code);
            pthread_detach(t);
            pthread_create(&t, NULL, jule_timeout_thread, code);
            pthread_detach(t);
            yed_force_update();
        }
    }

    jule_dirty = 0;
}

static void tge_plots(void);

static void after_jule(void) {
    yed_buffer *b;
    Jule_Value *table;
    Jule_Value *ID_str;
    Jule_Value *row;
    Jule_Value *ID_val;
    int         idx;
    Experiment *exp_p;

    b = yed_get_or_create_special_rdonly_buffer("*crapport-jule-output");

    b->flags &= ~BUFF_RD_ONLY;
    yed_buff_clear_no_undo(b);
    array_zero_term(jule_output_chars);
    yed_buff_insert_string_no_undo(b, array_data(jule_output_chars), 1, 1);
    array_clear(jule_output_chars);
    b->flags |= BUFF_RD_ONLY;

    pthread_mutex_lock(&experiments_lock);

    array_clear(experiments_working);

    table = jule_lookup(&interp, jule_get_string_id(&interp, "@table"));
    if (table == NULL)            { goto out_unlock; }
    if (table->type != JULE_LIST) { goto out_unlock; }

    ID_str = jule_string_value(&interp, "ID");

    FOR_EACH(table->list, row) {
        if (row == NULL)              { continue; }
        if (row->type != JULE_OBJECT) { continue; }

        ID_val = jule_field(row, ID_str);
        if (ID_val == NULL)              { continue; }
        if (ID_val->type != JULE_NUMBER) { continue; }

        idx = (int)ID_val->number;
        if (idx < 0 || idx >= array_len(experiments)) { continue; }

        exp_p = array_item(experiments, idx);
        array_push(experiments_working, *exp_p);
    }

    jule_free_value(ID_str);

out_unlock:;
    pthread_mutex_unlock(&experiments_lock);

    if (j_columns_str != NULL) {
        /* Setting this will update the buffer. Don't want to do it twice. */
        yed_set_var("crapport-columns", j_columns_str);
    } else {
        update_buffer();
    }

    tge_plots();

    jule_free(&interp);

    pthread_mutex_unlock(&jule_lock);

    if (has_err) {
        draw_error_message(1);
    }

    yed_force_update();
}

static void epump(yed_event *event) {
    u64 now;

    (void)event;

    if (jule_dirty) {
        now = measure_time_now_ms();
        if (now - jule_dirty_time_ms >= 500) {
            update_jule();
        }
    } else if (jule_finished) {
        after_jule();
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

static void draw_error_message(int);

static void estyle(yed_event *event) {
    yed_syntax_style_event(&syn, event);

    if (err_dd) {
        draw_error_message(0);
        draw_error_message(1);
    }
}
static void ebuffdel(yed_event *event) { yed_syntax_buffer_delete_event(&syn, event); }

static void ebuffmod(yed_event *event) {
    const char *jule;

    if (event->buffer == NULL) { return; }

    if (has_err && strcmp(event->buffer->name, err_file) == 0) {
        has_err   = 0;
        err_fixed = 1;
        draw_error_message(0);
    }

    if ((jule = yed_get_var("crapport-jule-file")) && strcmp(event->buffer->name, jule) == 0) {
        if (yed_var_is_truthy("crapport-jule-live-update")) {
            on_jule_update();
        }
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

static void erow(yed_event *event) {
    yed_buffer *buff;

    if (!has_err)               { return; }
    if (event->row != err_line) { return; }

    buff = yed_get_buffer_by_path(err_file);

    if (event->frame->buffer != buff) { return; }

    event->row_base_attr = get_err_attrs();
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
            (void)val;
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


static void free_plot(Plot *plot) {
    Plot_Point_Group *group;

    free(plot->title);
    array_traverse(plot->groups, group) {
        if (group->label != NULL) {
            free(group->label);
        }
        array_free(group->points);
    }
}

static void tge_frame(TGE_Game *tge);
static void teardown_tge(void);

#define TO_SCREEN_X(_x) \
    ((plot->width - inner_screen_width) + (int)((((_x) - plot->xmin) / plot_width) * inner_screen_width))
#define TO_SCREEN_Y(_y) \
    (plot->height - (plot->height - inner_screen_height) - (int)((((_y) - plot->ymin) / plot_height) * inner_screen_height))
#define IABS(_i) ((_i) < 0 ? -(_i) : (_i))

static void tge_point_group(TGE_Widget *widget, Plot *plot, Plot_Point_Group *group, int idx) {
    TGE_Canvas_Widget *canvas;
    TGE_Screen        *screen;
    double             plot_width;
    double             plot_height;
    int                inner_screen_width;
    int                inner_screen_height;
    int                min_screen_x;
    int                label_y_off;
    int                x_axis_x;
    int                y_axis_y;
    int                screen_size;
    Plot_Point        *last_point;
    Plot_Point        *point;
    int                screen_x;
    int                screen_y;
    int                x0;
    int                y0;
    int                x1;
    int                y1;
    int                dx;
    int                dy;
    int                N;
    float              divN;
    float              xstep;
    float              ystep;
    float              xf;
    float              yf;
    int                step;
    int                x;
    int                y;
    char               buff[128];

    canvas              = widget->data;
    screen              = &canvas->screen;
    plot_width          = plot->xmax - plot->xmin;
    plot_height         = plot->ymax - plot->ymin;
    inner_screen_width  = (int)((1.0 - plot->axis_pad) * plot->width);
    inner_screen_height = (int)((1.0 - plot->axis_pad) * plot->height);
    min_screen_x        = inner_screen_width;
    x_axis_x            = (int)(plot->axis_pad * plot->width);
    y_axis_y            = plot->height - (int)(plot->axis_pad * plot->height);
    screen_size         = MAX(1, (int)((group->size / plot_width) * inner_screen_width));

    if (inner_screen_height <= 0 || inner_screen_width <= 0) { return; }

    if (group->color == POINT_COLOR_NOT_SET) {
        group->color = plot->fg;
    }

    last_point = NULL;
    array_traverse(group->points, point) {
        screen_x = TO_SCREEN_X(point->x);
        screen_y = TO_SCREEN_Y(point->y);

        if (screen_y < 0 || screen_x < 0) { break; }

        min_screen_x = MIN(min_screen_x, screen_x);

        if (plot->point_labels) {
            if ((long long)point->y == point->y) {
                snprintf(buff, sizeof(buff), "%lld", (long long)point->y);
            } else {
                snprintf(buff, sizeof(buff), "%.3g", point->y);
            }

            label_y_off = -2;

            if (plot->type == PLOT_BAR && point->y < 0) {
                label_y_off *= -1;
            }

            tge_canvas_widget_add_label(widget, screen_x - (strlen(buff) / 2), screen_y + label_y_off, buff, plot->fg, -1, 0);
        }

        switch (plot->type) {
            case PLOT_SCATTER:
                if (screen_x != x_axis_x && screen_y != y_axis_y) {
                    tge_screen_set_pixel(screen, screen_x, screen_y, group->color);
                }
                break;
            case PLOT_LINE:
                if (last_point != NULL) {
                    x0   = TO_SCREEN_X(last_point->x);
                    y0   = TO_SCREEN_Y(last_point->y);
                    x1   = screen_x;
                    y1   = screen_y;
                    dx   = x1 - x0;
                    dy   = y1 - y0;
                    N    = MAX(IABS(dx), IABS(dy));
                    divN = N == 0 ? 0.0 : 1.0 / N;
                    xstep = dx * divN;
                    ystep = dy * divN;
                    xf    = x0;
                    yf    = y0;

                    for (step = 0; step <= N; step += 1, xf += xstep, yf += ystep) {
                        screen_x = (int)round(xf);
                        screen_y = (int)round(yf);

                        if (screen_x != x_axis_x && screen_y != y_axis_y) {
                            tge_screen_set_pixel(screen, screen_x, screen_y, group->color);
                        }
                    }

                } else {
                    if (screen_x != x_axis_x && screen_y != y_axis_y) {
                        tge_screen_set_pixel(screen, screen_x, screen_y, group->color);
                    }
                }
                break;
            case PLOT_BAR:
                if (point->y > 0) {
                    y = TO_SCREEN_Y(MAX(plot->ymin, 0));
                    for (y = y - (y == y_axis_y); y >= screen_y; y -= 1) {
                        for (x = 0; x < screen_size; x += 1) {
                            tge_screen_set_pixel(screen, screen_x - (screen_size / 2) + x, y, group->color);
                        }
                    }
                } else {
                    y = screen_y;
                    for (y = y - (y == y_axis_y); y > TO_SCREEN_Y(MIN(plot->ymax, 0)); y -= 1) {
                        for (x = 0; x < screen_size; x += 1) {
                            tge_screen_set_pixel(screen, screen_x - (screen_size / 2) + x, y, group->color);
                        }
                    }
                }
                break;
        }

        last_point = point;
    }

    if (group->label != NULL) {
        screen_x = isnan(group->labelx)
                        ? min_screen_x
                        : TO_SCREEN_X(group->labelx);
        screen_y = isnan(group->labely)
                        ? inner_screen_height + 4 + ((idx & 1) * 2)
                        : TO_SCREEN_Y(group->labely);

        if (plot->type == PLOT_BAR && idx & 1 && isnan(group->labely)) {
            tge_canvas_widget_add_label(widget, screen_x, screen_y - 2, "|", group->color, -1, TGE_LABEL_BOLD);
        }
        tge_canvas_widget_add_label(widget,
                                    screen_x - (strlen(group->label) / 2),
                                    screen_y,
                                    group->label,
                                    plot->invert_labels ? plot->bg     : group->color,
                                    plot->invert_labels ? group->color : plot->bg,
                                    TGE_LABEL_BOLD);
    }
}

static void tge_plots(void) {
    Plot              *plot;
    TGE_Widget        *widget;
    TGE_Canvas_Widget *canvas;
    TGE_Screen        *screen;
    Plot_Point_Group  *group;
    int                y;
    int                x;
    int                idx;
    char               buff[128];
    double             plot_width;
    double             plot_height;
    int                inner_screen_height;
    int                inner_screen_width;
    double             z;

    teardown_tge();

    if (array_len(jule_plots) == 0) { return; }

    tge = tge_new_game(Self, 0, 0, 3, TGE_TAKE_MOUSE);
    tge->frame_callback = tge_frame;

    array_traverse(jule_plots, plot) {
        widget = tge_new_canvas_widget(tge, plot->width, plot->height, plot->title);
        canvas = widget->data;
        screen = &canvas->screen;

        if (plot->appearx > 0) {
            widget->hitbox_left = plot->appearx;
        }
        if (plot->appeary > 0) {
            widget->hitbox_top = plot->appeary;
        }

        for (y = 0; y < plot->height; y += 1) {
            for (x = 0; x < plot->width; x += 1) {
                tge_screen_set_pixel(screen, x, y, plot->bg);
            }
        }

        y = plot->height - (int)(plot->axis_pad * plot->height);
        for (x = (int)(plot->axis_pad * plot->width); x <= plot->width; x += 1) {
            tge_screen_set_pixel(screen, x, y, plot->fg);
        }
        x = (int)(plot->axis_pad * plot->width);
        for (y = plot->height - (int)(plot->axis_pad * plot->height); y >= 0; y -= 1) {
            tge_screen_set_pixel(screen, x, y, plot->fg);
        }

        plot_width          = plot->xmax - plot->xmin;
        plot_height         = plot->ymax - plot->ymin;
        inner_screen_height = (int)((1.0 - plot->axis_pad) * screen->height);
        inner_screen_width  = (int)((1.0 - plot->axis_pad) * screen->width);
        y                   = inner_screen_height;

        if (plot->ymarks > 1) {
            for (z = plot->ymin; z <= plot->ymax; z += ((plot->ymax - plot->ymin) / plot->ymarks)) {
                y = TO_SCREEN_Y(z);

                if ((long long)z == z) {
                    snprintf(buff, sizeof(buff), "%lld ─", (long long)z);
                } else {
                    snprintf(buff, sizeof(buff), "%.3g ─", z);
                }

                tge_canvas_widget_add_label(widget,
                                            (int)(plot->axis_pad * plot->width) - yed_get_string_width(buff),
                                            y,
                                            buff,
                                            plot->fg, plot->bg,
                                            0);
            }
        }
        if (plot->xmarks > 1) {
            x = (int)(plot->axis_pad * plot->width);
            y = plot->height - (int)(plot->axis_pad * plot->height) + 2;
            for (z = plot->xmin; z <= plot->xmax; z += ((plot->xmax - plot->xmin) / plot->xmarks)) {
                x = TO_SCREEN_X(z);

                if (z >= plot->xmax) {
                    x -= 1;
                }

                snprintf(buff, sizeof(buff), "╵");
                tge_canvas_widget_add_label(widget,
                                            x,
                                            y,
                                            buff,
                                            plot->fg, plot->bg,
                                            0);
                if ((long long)z == z) {
                    snprintf(buff, sizeof(buff), "%lld", (long long)z);
                } else {
                    snprintf(buff, sizeof(buff), "%.3g", z);
                }
                tge_canvas_widget_add_label(widget,
                                            x - (yed_get_string_width(buff) / (z >= plot->xmax ? 1 : 2)),
                                            y + 2,
                                            buff,
                                            plot->fg, plot->bg,
                                            0);
            }
        }

        idx = 0;
        array_traverse(plot->groups, group) {
            tge_point_group(widget, plot, group, idx);
            idx += 1;
        }

        free_plot(plot);
    }

    array_clear(jule_plots);
}

static void teardown_tge(void) {
    if (tge != NULL) {
        tge_finish_game(tge);
    }
    tge = NULL;
}

static void tge_frame(TGE_Game *tge) {
    if (array_len(tge->widgets) == 0) {
        teardown_tge();
        return;
    }
}

static void unload(yed_plugin *self) {
    (void)self;
    free_all();
    /* @todo */
/*     yed_free_buffer(yed_get_or_create_special_rdonly_buffer(BUFFER_NAME)); */
    yed_syntax_free(&syn);

    teardown_tge();
}

int yed_plugin_boot(yed_plugin *self) {
    yed_event_handler pump_handler;
    yed_event_handler clear_handler;
    yed_event_handler style_handler;
    yed_event_handler buffdel_handler;
    yed_event_handler buffmod_handler;
    yed_event_handler line_handler;
    yed_event_handler row_handler;
    yed_event_handler var_handler;
    yed_event_handler load_handler;
    yed_event_handler write_handler;


    YED_PLUG_VERSION_CHECK();

    Self = self;

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

    row_handler.kind = EVENT_ROW_PRE_CLEAR;
    row_handler.fn   = erow;
    yed_plugin_add_event_handler(self, row_handler);

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
    if (yed_get_var("crapport-jule-live-update") == NULL) {
        yed_set_var("crapport-jule-live-update", "no");
    }

    yed_set_var("crapport-debug-log", "yes");

    yed_get_or_create_special_rdonly_buffer(BUFFER_NAME);
    yed_get_or_create_special_rdonly_buffer("*crapport-jule-output");

    return 0;
}
