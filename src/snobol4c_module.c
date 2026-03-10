/*======================================================================================
 * snobol4c_module.c — CPython extension: SNOBOL4 pattern match engine (v2)
 *
 * Python builds the PATTERN tree.  C runs the match engine.  Python gets the result.
 *
 * Memory: malloc/realloc throughout.  No arenas, no fixed limits.
 *   - realloc(NULL, size) = malloc
 *   - realloc(p, 0)       = free
 *
 * Build:
 *   python3 setup.py build_ext --inplace
 *======================================================================================*/
#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>

/*======================================================================================
 * PATTERN types
 *======================================================================================*/
enum {
    T_ABORT   =  0,
    T_ANY     =  1,
    T_ARB     =  2,
    T_ARBNO   =  3,
    T_BAL     =  4,
    T_BREAK   =  5,
    T_BREAKX  =  6,
    T_FAIL    =  7,
    T_FENCE   =  8,
    T_LEN     =  9,
    T_MARB    = 10,
    T_MARBNO  = 11,
    T_NOTANY  = 12,
    T_POS     = 13,
    T_REM     = 14,
    T_RPOS    = 15,
    T_RTAB    = 16,
    T_SPAN    = 17,
    T_SUCCEED = 18,
    T_TAB     = 19,
    T_PI      = 29,     /* Π  alternation  */
    T_SIGMA   = 30,     /* Σ  sequence     */
    T_RHO     = 39,     /* ρ  conjunction  */
    T_pi      = 38,     /* π  optional     */
    T_EPSILON = 34,     /* ε  null match   */
    T_LITERAL = 40,     /* σ  literal str  */
    T_ALPHA   = 32,     /* α  start of line */
    T_OMEGA   = 42,     /* ω  end of line  */
};

#define PROCEED 0
#define SUCCESS 1
#define FAILURE 2
#define RECEDE  3

/*======================================================================================
 * C PATTERN node — malloc'd from Python tree
 *======================================================================================*/
#define MAX_CHILDREN 30

typedef struct Pattern {
    int                 type;
    int                 n;
    const char *        s;
    Py_ssize_t          s_len;
    const char *        chars;
    struct Pattern *    children[MAX_CHILDREN];
} Pattern;

/*--- Pattern tracking: realloc'd list of all malloc'd nodes for cleanup ---*/
typedef struct {
    Pattern **  list;
    int         count;
} PatternList;

static Pattern *pattern_alloc(PatternList *pl) {
    pl->list = realloc(pl->list, (pl->count + 1) * sizeof(Pattern *));
    Pattern *p = malloc(sizeof(Pattern));
    memset(p, 0, sizeof(Pattern));
    pl->list[pl->count++] = p;
    return p;
}

static void pattern_free_all(PatternList *pl) {
    for (int i = 0; i < pl->count; i++)
        free(pl->list[i]);
    free(pl->list);
    pl->list  = NULL;
    pl->count = 0;
}

/*======================================================================================
 * Python → C PATTERN tree converter
 *======================================================================================*/
static const char *py_class_name(PyObject *obj) {
    PyObject *tp = (PyObject *)Py_TYPE(obj);
    PyObject *name = PyObject_GetAttrString(tp, "__name__");
    if (!name) return NULL;
    const char *s = PyUnicode_AsUTF8(name);
    Py_DECREF(name);
    return s;
}

static int py_get_int(PyObject *obj, const char *attr) {
    PyObject *val = PyObject_GetAttrString(obj, attr);
    if (!val) return 0;
    int n = (int)PyLong_AsLong(val);
    Py_DECREF(val);
    return n;
}

static const char *py_get_str(PyObject *obj, const char *attr, Py_ssize_t *len) {
    PyObject *val = PyObject_GetAttrString(obj, attr);
    if (!val) return NULL;
    const char *s = PyUnicode_AsUTF8AndSize(val, len);
    Py_DECREF(val);
    return s;
}

static Pattern *convert(PatternList *pl, PyObject *py_pat);

static int convert_children(PatternList *pl, Pattern *p, PyObject *py_pat) {
    PyObject *AP = PyObject_GetAttrString(py_pat, "AP");
    if (!AP) return -1;
    Py_ssize_t n = PyTuple_Size(AP);
    if (n > MAX_CHILDREN) n = MAX_CHILDREN;
    p->n = (int)n;
    for (Py_ssize_t i = 0; i < n; i++) {
        p->children[i] = convert(pl, PyTuple_GetItem(AP, i));
        if (!p->children[i]) { Py_DECREF(AP); return -1; }
    }
    Py_DECREF(AP);
    return 0;
}

static int convert_single_child(PatternList *pl, Pattern *p, PyObject *py_pat) {
    PyObject *child = PyObject_GetAttrString(py_pat, "P");
    if (!child || child == Py_None) {
        Py_XDECREF(child);
        p->n = 0;
        return 0;
    }
    p->n = 1;
    p->children[0] = convert(pl, child);
    Py_DECREF(child);
    return p->children[0] ? 0 : -1;
}

static Pattern *convert(PatternList *pl, PyObject *py_pat) {
    Pattern *p = pattern_alloc(pl);
    if (!p) { PyErr_NoMemory(); return NULL; }

    const char *name = py_class_name(py_pat);
    if (!name) return NULL;

    if      (strcmp(name, "\xcf\x83") == 0) {               /* σ */
        p->type = T_LITERAL;
        p->s = py_get_str(py_pat, "s", &p->s_len);
    }
    else if (strcmp(name, "POS") == 0)     { p->type = T_POS;    p->n = py_get_int(py_pat, "n"); }
    else if (strcmp(name, "RPOS") == 0)    { p->type = T_RPOS;   p->n = py_get_int(py_pat, "n"); }
    else if (strcmp(name, "LEN") == 0)     { p->type = T_LEN;    p->n = py_get_int(py_pat, "n"); }
    else if (strcmp(name, "TAB") == 0)     { p->type = T_TAB;    p->n = py_get_int(py_pat, "n"); }
    else if (strcmp(name, "RTAB") == 0)    { p->type = T_RTAB;   p->n = py_get_int(py_pat, "n"); }
    else if (strcmp(name, "ANY") == 0)     { p->type = T_ANY;    Py_ssize_t d; p->chars = py_get_str(py_pat, "chars", &d); }
    else if (strcmp(name, "NOTANY") == 0)  { p->type = T_NOTANY; Py_ssize_t d; p->chars = py_get_str(py_pat, "chars", &d); }
    else if (strcmp(name, "SPAN") == 0)    { p->type = T_SPAN;   Py_ssize_t d; p->chars = py_get_str(py_pat, "chars", &d); }
    else if (strcmp(name, "BREAK") == 0)   { p->type = T_BREAK;  Py_ssize_t d; p->chars = py_get_str(py_pat, "chars", &d); }
    else if (strcmp(name, "BREAKX") == 0)  { p->type = T_BREAK;  Py_ssize_t d; p->chars = py_get_str(py_pat, "chars", &d); }
    else if (strcmp(name, "\xce\xa3") == 0) {                   /* Σ */
        p->type = T_SIGMA;
        if (convert_children(pl, p, py_pat) < 0) return NULL;
    }
    else if (strcmp(name, "\xce\xa0") == 0) {                   /* Π */
        p->type = T_PI;
        if (convert_children(pl, p, py_pat) < 0) return NULL;
    }
    else if (strcmp(name, "\xcf\x81") == 0) {                   /* ρ */
        p->type = T_RHO;
        if (convert_children(pl, p, py_pat) < 0) return NULL;
    }
    else if (strcmp(name, "\xcf\x80") == 0) {                   /* π */
        p->type = T_pi;
        if (convert_single_child(pl, p, py_pat) < 0) return NULL;
    }
    else if (strcmp(name, "ARBNO") == 0) {
        p->type = T_ARBNO;
        if (convert_single_child(pl, p, py_pat) < 0) return NULL;
    }
    else if (strcmp(name, "FENCE") == 0) {
        p->type = T_FENCE;
        if (convert_single_child(pl, p, py_pat) < 0) return NULL;
    }
    else if (strcmp(name, "\xce\xb5") == 0)  p->type = T_EPSILON;  /* ε */
    else if (strcmp(name, "\xce\xb1") == 0)  p->type = T_ALPHA;    /* α */
    else if (strcmp(name, "\xcf\x89") == 0)  p->type = T_OMEGA;    /* ω */
    else if (strcmp(name, "ARB") == 0)       p->type = T_ARB;
    else if (strcmp(name, "BAL") == 0)       p->type = T_BAL;
    else if (strcmp(name, "REM") == 0)       p->type = T_REM;
    else if (strcmp(name, "FAIL") == 0)      p->type = T_FAIL;
    else if (strcmp(name, "ABORT") == 0)     p->type = T_ABORT;
    else if (strcmp(name, "SUCCEED") == 0)   p->type = T_SUCCEED;
    else if (strcmp(name, "MARB") == 0)      p->type = T_MARB;
    else {
        PyErr_Format(PyExc_TypeError, "snobol4c: unsupported pattern type '%s'", name);
        return NULL;
    }
    return p;
}

/*======================================================================================
 * Psi — realloc'd array.  Plain push/pop stack.  Deep-copied into omega snapshots.
 *======================================================================================*/
typedef struct {
    Pattern *   PI;
    int         ctx;
} PsiEntry;

typedef struct {
    PsiEntry *  entries;    /* realloc'd with doubling */
    int         count;
    int         capacity;
} Psi;

static inline void psi_push(Psi *psi, Pattern *PI, int ctx) {
    if (psi->count >= psi->capacity) {
        psi->capacity = psi->capacity ? psi->capacity * 2 : 8;
        psi->entries = realloc(psi->entries, psi->capacity * sizeof(PsiEntry));
    }
    psi->entries[psi->count++] = (PsiEntry){PI, ctx};
}

static inline bool psi_pop(Psi *psi, Pattern **PI, int *ctx) {
    if (psi->count > 0) {
        *PI  = psi->entries[--psi->count].PI;
        *ctx = psi->entries[  psi->count].ctx;
        return true;
    }
    return false;
}

static inline Psi psi_snapshot(Psi *psi) {
    Psi snap = {NULL, psi->count, psi->count};
    if (psi->count > 0) {
        snap.entries = malloc(psi->count * sizeof(PsiEntry));
        memcpy(snap.entries, psi->entries, psi->count * sizeof(PsiEntry));
    }
    return snap;
}

static inline void psi_restore(Psi *psi, Psi *snap) {
    if (snap->count > psi->capacity) {
        psi->capacity = snap->count;
        psi->entries = realloc(psi->entries, psi->capacity * sizeof(PsiEntry));
    }
    psi->count = snap->count;
    if (snap->count > 0)
        memcpy(psi->entries, snap->entries, snap->count * sizeof(PsiEntry));
}

/*======================================================================================
 * State — engine working state (psi is separate)
 *======================================================================================*/
typedef struct {
    const char *    SIGMA;
    int             DELTA;
    int             OMEGA;
    const char *    sigma;
    int             delta;
    Pattern *       PI;
    int             fenced;
    int             yielded;
    int             ctx;
} State;

/*======================================================================================
 * Omega — realloc'd backtrack stack.  Each entry owns a psi snapshot.
 *======================================================================================*/
typedef struct {
    State   state;
    Psi     psi_snap;   /* owned copy */
} OmegaEntry;

typedef struct {
    OmegaEntry *    entries;    /* realloc'd with doubling */
    int             count;
    int             capacity;
} Omega;

static void omega_push(Omega *omega, State *z, Psi *psi) {
    if (omega->count >= omega->capacity) {
        omega->capacity = omega->capacity ? omega->capacity * 2 : 8;
        omega->entries = realloc(omega->entries, omega->capacity * sizeof(OmegaEntry));
    }
    omega->entries[omega->count].state    = *z;
    omega->entries[omega->count].psi_snap = psi_snapshot(psi);
    omega->count++;
}

static void omega_pop(Omega *omega, State *z, Psi *psi) {
    if (omega->count > 0) {
        OmegaEntry *e = &omega->entries[--omega->count];
        *z = e->state;
        psi_restore(psi, &e->psi_snap);
        free(e->psi_snap.entries);
        e->psi_snap.entries = NULL;
    } else {
        memset(z, 0, sizeof(State));
        z->PI = NULL;
        psi->count = 0;
    }
}

static OmegaEntry *omega_tip(Omega *omega) {
    return omega->count > 0 ? &omega->entries[omega->count - 1] : NULL;
}

static void omega_free(Omega *omega) {
    for (int i = 0; i < omega->count; i++)
        free(omega->entries[i].psi_snap.entries);
    free(omega->entries);
    omega->entries  = NULL;
    omega->count    = 0;
    omega->capacity = 0;
}

/*======================================================================================
 * State navigation
 *======================================================================================*/
static inline void z_down(State *z, Psi *psi) {
    psi_push(psi, z->PI, z->ctx);
    z->sigma = z->SIGMA;
    z->delta = z->DELTA;
    z->PI    = z->PI->children[z->ctx];
    z->ctx   = 0;
}

static inline void z_down_single(State *z, Psi *psi) {
    psi_push(psi, z->PI, z->ctx);
    z->sigma = z->SIGMA;
    z->delta = z->DELTA;
    z->PI    = z->PI->children[0];
    z->ctx   = 0;
}

static inline void z_up(State *z, Psi *psi) {
    if (!psi_pop(psi, &z->PI, &z->ctx))
        z->PI = NULL;
}

static inline void z_up_track(State *z, Psi *psi, Omega *omega) {
    OmegaEntry *track = omega_tip(omega);
    if (track) {
        track->state.SIGMA   = z->SIGMA;
        track->state.DELTA   = z->DELTA;
        track->state.sigma   = z->sigma;
        track->state.delta   = z->delta;
        track->state.yielded = 1;
    }
    z_up(z, psi);
}

static inline void z_up_fail(State *z, Psi *psi) {
    z_up(z, psi);
}

static inline void z_stay_next(State *z) {
    z->sigma   = z->SIGMA;
    z->delta   = z->DELTA;
    z->yielded = 0;
    z->ctx++;
}

static inline void z_move_next(State *z) {
    z->SIGMA   = z->sigma;
    z->DELTA   = z->delta;
    z->yielded = 0;
    z->ctx++;
}

static inline void z_next(State *z) {
    z->sigma = z->SIGMA;
    z->delta = z->DELTA;
}

/*======================================================================================
 * Pattern scanners
 *======================================================================================*/
static inline bool scan_move(State *z, int delta) {
    if (delta >= 0 && z->DELTA + delta <= z->OMEGA) {
        z->sigma += delta;
        z->delta += delta;
        return true;
    }
    return false;
}

static bool scan_LITERAL(State *z) {
    if (z->delta + z->PI->s_len > z->OMEGA) return false;
    if (memcmp(z->sigma, z->PI->s, z->PI->s_len) != 0) return false;
    z->sigma += z->PI->s_len;
    z->delta += z->PI->s_len;
    return true;
}

static bool scan_ANY(State *z) {
    if (z->delta >= z->OMEGA) return false;
    for (const char *c = z->PI->chars; *c; c++)
        if (*z->sigma == *c) { z->sigma++; z->delta++; return true; }
    return false;
}

static bool scan_NOTANY(State *z) {
    if (z->delta >= z->OMEGA) return false;
    for (const char *c = z->PI->chars; *c; c++)
        if (*z->sigma == *c) return false;
    z->sigma++; z->delta++; return true;
}

static bool scan_SPAN(State *z) {
    int start = z->delta;
    while (z->delta < z->OMEGA) {
        const char *c;
        for (c = z->PI->chars; *c; c++)
            if (*z->sigma == *c) break;
        if (!*c) break;
        z->sigma++; z->delta++;
    }
    return z->delta > start;
}

static bool scan_BREAK(State *z) {
    while (z->delta < z->OMEGA) {
        for (const char *c = z->PI->chars; *c; c++)
            if (*z->sigma == *c) return true;
        z->sigma++; z->delta++;
    }
    return false;
}

static bool scan_ARB(State *z) {
    if (z->DELTA + z->ctx <= z->OMEGA) {
        z->sigma += z->ctx;
        z->delta += z->ctx;
        return true;
    }
    return false;
}

static bool scan_BAL(State *z) {
    int nest = 0;
    z->sigma += z->ctx + 1;
    z->delta += z->ctx + 1;
    while (z->delta <= z->OMEGA) {
        char ch = z->sigma[-1];
        if      (ch == '(') nest++;
        else if (ch == ')') nest--;
        if (nest < 0) break;
        else if (nest > 0 && z->delta >= z->OMEGA) break;
        else if (nest == 0) { z->ctx = z->delta; return true; }
        z->sigma++; z->delta++;
    }
    return false;
}

static inline bool scan_POS(State *z)   { return z->PI->n == z->DELTA; }
static inline bool scan_RPOS(State *z)  { return z->PI->n == z->OMEGA - z->DELTA; }
static inline bool scan_LEN(State *z)   { return scan_move(z, z->PI->n); }
static inline bool scan_TAB(State *z)   { return scan_move(z, z->PI->n - z->DELTA); }
static inline bool scan_REM(State *z)   { return scan_move(z, z->OMEGA - z->DELTA); }
static inline bool scan_RTAB(State *z)  { return scan_move(z, z->OMEGA - z->DELTA - z->PI->n); }
static inline bool scan_ALPHA(State *z) { return z->DELTA == 0 || (z->DELTA > 0 && z->SIGMA[-1] == '\n'); }
static inline bool scan_OMEGA(State *z) { return z->DELTA == z->OMEGA || (z->DELTA < z->OMEGA && z->SIGMA[0] == '\n'); }

/*======================================================================================
 * THE MATCH ENGINE
 *======================================================================================*/
typedef struct {
    int matched;
    int start;
    int end;
} MatchResult;

static MatchResult engine_match(Pattern *pattern, const char *subject, int subject_len) {
    MatchResult result = {0, 0, 0};

    Psi   psi   = {NULL, 0, 0};
    Omega omega = {NULL, 0, 0};

    int a = PROCEED;
    State Z;
    memset(&Z, 0, sizeof(Z));
    Z.SIGMA = subject;
    Z.OMEGA = subject_len;
    Z.sigma = subject;
    Z.PI    = pattern;

    while (Z.PI) {
        int t = Z.PI->type;
        switch (t << 2 | a) {
/*--- Π (alternation) ---------------------------------------------------------------*/
        case T_PI<<2|PROCEED:
            if (Z.ctx < Z.PI->n) { a = PROCEED; omega_push(&omega, &Z, &psi); z_down(&Z, &psi);              break; }
            else                 { a = RECEDE;   omega_pop(&omega, &Z, &psi);                                  break; }
        case T_PI<<2|SUCCESS:    { a = SUCCESS;                                 z_up(&Z, &psi);                break; }
        case T_PI<<2|FAILURE:    { a = PROCEED;                                 z_stay_next(&Z);               break; }
        case T_PI<<2|RECEDE:
            if (!Z.fenced)       { a = PROCEED;                                 z_stay_next(&Z);               break; }
            else                 { a = FAILURE;                                 z_up_fail(&Z, &psi);           break; }
/*--- Σ (sequence) ------------------------------------------------------------------*/
        case T_SIGMA<<2|PROCEED:
            if (Z.ctx < Z.PI->n) { a = PROCEED;                                z_down(&Z, &psi);              break; }
            else                 { a = SUCCESS;                                 z_up(&Z, &psi);                break; }
        case T_SIGMA<<2|SUCCESS: { a = PROCEED;                                 z_move_next(&Z);               break; }
        case T_SIGMA<<2|FAILURE: { a = RECEDE;   omega_pop(&omega, &Z, &psi);                                  break; }
/*--- ρ (conjunction) ---------------------------------------------------------------*/
        case T_RHO<<2|PROCEED:
            if (Z.ctx < Z.PI->n) { a = PROCEED;                                z_down(&Z, &psi);              break; }
            else                 { a = SUCCESS;                                 z_up(&Z, &psi);                break; }
        case T_RHO<<2|SUCCESS:   { a = PROCEED;                                 z_stay_next(&Z);               break; }
        case T_RHO<<2|FAILURE:   { a = RECEDE;   omega_pop(&omega, &Z, &psi);                                  break; }
/*--- π (optional) ------------------------------------------------------------------*/
        case T_pi<<2|PROCEED:
            if (Z.ctx == 0)      { a = SUCCESS;  omega_push(&omega, &Z, &psi); z_up(&Z, &psi);                break; }
            else if (Z.ctx == 1) { a = PROCEED;  omega_push(&omega, &Z, &psi); z_down_single(&Z, &psi);       break; }
            else                 { a = RECEDE;   omega_pop(&omega, &Z, &psi);                                  break; }
        case T_pi<<2|SUCCESS:    { a = SUCCESS;                                 z_up(&Z, &psi);                break; }
        case T_pi<<2|FAILURE:    { a = FAILURE;                                 z_up_fail(&Z, &psi);           break; }
        case T_pi<<2|RECEDE:
            if (!Z.fenced)       { a = PROCEED;                                 z_stay_next(&Z);               break; }
            else                 { a = FAILURE;                                 z_up_fail(&Z, &psi);           break; }
/*--- ARBNO -------------------------------------------------------------------------*/
        case T_ARBNO<<2|PROCEED:
            if (Z.ctx == 0)      { a = SUCCESS;  omega_push(&omega, &Z, &psi); z_up_track(&Z, &psi, &omega);  break; }
            else                 { a = PROCEED;  omega_push(&omega, &Z, &psi); z_down_single(&Z, &psi);       break; }
        case T_ARBNO<<2|SUCCESS: { a = SUCCESS;                                 z_up_track(&Z, &psi, &omega);  break; }
        case T_ARBNO<<2|FAILURE: { a = RECEDE;   omega_pop(&omega, &Z, &psi);                                  break; }
        case T_ARBNO<<2|RECEDE:
            if (Z.fenced)        { a = FAILURE;                                 z_up_fail(&Z, &psi);           break; }
            else if (Z.yielded)  { a = PROCEED;                                 z_move_next(&Z);               break; }
            else                 { a = FAILURE;                                 z_up_fail(&Z, &psi);           break; }
/*--- ARB ---------------------------------------------------------------------------*/
        case T_ARB<<2|PROCEED:
            if (scan_ARB(&Z))    { a = SUCCESS;  omega_push(&omega, &Z, &psi); z_up(&Z, &psi);                break; }
            else                 { a = RECEDE;   omega_pop(&omega, &Z, &psi);                                  break; }
        case T_ARB<<2|RECEDE:
            if (!Z.fenced)       { a = PROCEED;                                 z_stay_next(&Z);               break; }
            else                 { a = FAILURE;                                 z_up_fail(&Z, &psi);           break; }
/*--- BAL ---------------------------------------------------------------------------*/
        case T_BAL<<2|PROCEED:
            if (scan_BAL(&Z))    { a = SUCCESS;  omega_push(&omega, &Z, &psi); z_up(&Z, &psi);                break; }
            else                 { a = RECEDE;   omega_pop(&omega, &Z, &psi);                                  break; }
        case T_BAL<<2|RECEDE:
            if (!Z.fenced)       { a = PROCEED;                                 z_next(&Z);                    break; }
            else                 { a = FAILURE;                                 z_up_fail(&Z, &psi);           break; }
/*--- FENCE -------------------------------------------------------------------------*/
        case T_FENCE<<2|PROCEED:
            if (Z.PI->n == 0)    { a = SUCCESS;  omega_push(&omega, &Z, &psi); z_up(&Z, &psi);                break; }
            else                 { a = PROCEED;  Z.fenced = 1;                  z_down_single(&Z, &psi);       break; }
        case T_FENCE<<2|RECEDE:
            if (Z.PI->n == 0)    { a = RECEDE;   Z.PI = NULL;                                                  break; }
            else                 { a = FAILURE;   Z.PI = NULL;                                                  break; }
        case T_FENCE<<2|SUCCESS:
            if (Z.PI->n == 1)    { a = SUCCESS;  Z.fenced = 0;                  z_up(&Z, &psi);               break; }
            else                 { a = FAILURE;   Z.PI = NULL;                                                  break; }
        case T_FENCE<<2|FAILURE:
            if (Z.PI->n == 1)    { a = FAILURE;  Z.fenced = 0;                  z_up_fail(&Z, &psi);          break; }
            else                 { a = FAILURE;   Z.PI = NULL;                                                  break; }
/*--- Control -----------------------------------------------------------------------*/
        case T_ABORT<<2|PROCEED:   { a = FAILURE;  Z.PI = NULL;                                                break; }
        case T_SUCCEED<<2|PROCEED: { a = SUCCESS;  omega_push(&omega, &Z, &psi); z_up(&Z, &psi);              break; }
        case T_SUCCEED<<2|RECEDE:
            if (!Z.fenced)         { a = PROCEED;                                z_stay_next(&Z);              break; }
            else                   { a = FAILURE;                                z_up_fail(&Z, &psi);          break; }
        case T_FAIL<<2|PROCEED:    { a = FAILURE;                                z_up_fail(&Z, &psi);          break; }
        case T_EPSILON<<2|PROCEED: { a = SUCCESS;                                z_up(&Z, &psi);               break; }
/*--- Leaf scanners -----------------------------------------------------------------*/
        case T_LITERAL<<2|PROCEED:
            if (scan_LITERAL(&Z))  { a = SUCCESS; z_up(&Z, &psi); } else { a = FAILURE; z_up_fail(&Z, &psi); } break;
        case T_ANY<<2|PROCEED:
            if (scan_ANY(&Z))      { a = SUCCESS; z_up(&Z, &psi); } else { a = FAILURE; z_up_fail(&Z, &psi); } break;
        case T_NOTANY<<2|PROCEED:
            if (scan_NOTANY(&Z))   { a = SUCCESS; z_up(&Z, &psi); } else { a = FAILURE; z_up_fail(&Z, &psi); } break;
        case T_SPAN<<2|PROCEED:
            if (scan_SPAN(&Z))     { a = SUCCESS; z_up(&Z, &psi); } else { a = FAILURE; z_up_fail(&Z, &psi); } break;
        case T_BREAK<<2|PROCEED:
            if (scan_BREAK(&Z))    { a = SUCCESS; z_up(&Z, &psi); } else { a = FAILURE; z_up_fail(&Z, &psi); } break;
        case T_POS<<2|PROCEED:
            if (scan_POS(&Z))      { a = SUCCESS; z_up(&Z, &psi); } else { a = FAILURE; z_up_fail(&Z, &psi); } break;
        case T_RPOS<<2|PROCEED:
            if (scan_RPOS(&Z))     { a = SUCCESS; z_up(&Z, &psi); } else { a = FAILURE; z_up_fail(&Z, &psi); } break;
        case T_LEN<<2|PROCEED:
            if (scan_LEN(&Z))      { a = SUCCESS; z_up(&Z, &psi); } else { a = FAILURE; z_up_fail(&Z, &psi); } break;
        case T_TAB<<2|PROCEED:
            if (scan_TAB(&Z))      { a = SUCCESS; z_up(&Z, &psi); } else { a = FAILURE; z_up_fail(&Z, &psi); } break;
        case T_RTAB<<2|PROCEED:
            if (scan_RTAB(&Z))     { a = SUCCESS; z_up(&Z, &psi); } else { a = FAILURE; z_up_fail(&Z, &psi); } break;
        case T_REM<<2|PROCEED:
            if (scan_REM(&Z))      { a = SUCCESS; z_up(&Z, &psi); } else { a = FAILURE; z_up_fail(&Z, &psi); } break;
        case T_ALPHA<<2|PROCEED:
            if (scan_ALPHA(&Z))    { a = SUCCESS; z_up(&Z, &psi); } else { a = FAILURE; z_up_fail(&Z, &psi); } break;
        case T_OMEGA<<2|PROCEED:
            if (scan_OMEGA(&Z))    { a = SUCCESS; z_up(&Z, &psi); } else { a = FAILURE; z_up_fail(&Z, &psi); } break;
/*--- MARB --------------------------------------------------------------------------*/
        case T_MARB<<2|PROCEED:
            if (scan_ARB(&Z))      { a = SUCCESS; omega_push(&omega, &Z, &psi); z_up(&Z, &psi); break; }
            else                   { a = RECEDE;  omega_pop(&omega, &Z, &psi);                   break; }
        case T_MARB<<2|RECEDE:
            if (!Z.fenced)         { a = PROCEED;                                z_stay_next(&Z); break; }
            else                   { a = FAILURE;                                z_up_fail(&Z, &psi); break; }
/*-----------------------------------------------------------------------------------*/
        default:
            a = FAILURE;
            Z.PI = NULL;
            break;
        }
    }

    if (a == SUCCESS) {
        result.matched = 1;
        result.start   = 0;
        result.end     = Z.delta;
    }

    /* Cleanup */
    free(psi.entries);
    omega_free(&omega);

    return result;
}

/*======================================================================================
 * CPython module
 *======================================================================================*/
static PyObject *py_match(PyObject *self, PyObject *args) {
    PyObject *py_pattern;
    const char *subject;
    Py_ssize_t subject_len;

    if (!PyArg_ParseTuple(args, "Os#", &py_pattern, &subject, &subject_len))
        return NULL;

    PatternList pl = {NULL, 0};
    Pattern *root = convert(&pl, py_pattern);
    if (!root) { pattern_free_all(&pl); return NULL; }

    MatchResult result = engine_match(root, subject, (int)subject_len);
    pattern_free_all(&pl);

    if (result.matched)
        return Py_BuildValue("(ii)", result.start, result.end);
    Py_RETURN_NONE;
}

static PyObject *py_search(PyObject *self, PyObject *args) {
    PyObject *py_pattern;
    const char *subject;
    Py_ssize_t subject_len;

    if (!PyArg_ParseTuple(args, "Os#", &py_pattern, &subject, &subject_len))
        return NULL;

    PatternList pl = {NULL, 0};
    Pattern *root = convert(&pl, py_pattern);
    if (!root) { pattern_free_all(&pl); return NULL; }

    for (Py_ssize_t start = 0; start <= subject_len; start++) {
        MatchResult result = engine_match(root, subject + start, (int)(subject_len - start));
        if (result.matched) {
            pattern_free_all(&pl);
            return Py_BuildValue("(ii)", (int)start, (int)start + result.end);
        }
    }

    pattern_free_all(&pl);
    Py_RETURN_NONE;
}

static PyObject *py_fullmatch(PyObject *self, PyObject *args) {
    PyObject *py_pattern;
    const char *subject;
    Py_ssize_t subject_len;

    if (!PyArg_ParseTuple(args, "Os#", &py_pattern, &subject, &subject_len))
        return NULL;

    PatternList pl = {NULL, 0};
    Pattern *root = convert(&pl, py_pattern);
    if (!root) { pattern_free_all(&pl); return NULL; }

    MatchResult result = engine_match(root, subject, (int)subject_len);
    pattern_free_all(&pl);

    if (result.matched && result.end == (int)subject_len)
        return Py_BuildValue("(ii)", result.start, result.end);
    Py_RETURN_NONE;
}

/*======================================================================================
 * Module definition
 *======================================================================================*/
static PyMethodDef snobol4c_methods[] = {
    {"match",     py_match,     METH_VARARGS, "match(pattern, subject) -> (start, end) or None"},
    {"search",    py_search,    METH_VARARGS, "search(pattern, subject) -> (start, end) or None"},
    {"fullmatch", py_fullmatch, METH_VARARGS, "fullmatch(pattern, subject) -> (start, end) or None"},
    {NULL, NULL, 0, NULL}
};

static struct PyModuleDef snobol4c_module = {
    PyModuleDef_HEAD_INIT,
    "snobol4c",
    "SNOBOL4 pattern matching engine (C extension)",
    -1,
    snobol4c_methods
};

PyMODINIT_FUNC PyInit_snobol4c(void) {
    return PyModule_Create(&snobol4c_module);
}
