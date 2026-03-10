/*======================================================================================
 * snobol4c_module.c — CPython extension: SNOBOL4 pattern match engine
 *
 * Python builds the PATTERN tree.  C runs the match engine.  Python gets the result.
 *
 * Build:
 *   python3 setup.py build_ext --inplace
 *   — or —
 *   gcc -shared -fPIC -O2 -o snobol4c$(python3-config --extension-suffix) \
 *       snobol4c_module.c $(python3-config --includes --ldflags)
 *======================================================================================*/
#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <stdbool.h>

/*======================================================================================
 * PATTERN types — same enum as SNOBOL4c.c
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
    /* --- composites --- */
    T_PI      = 29,     /* Π  alternation  */
    T_SIGMA   = 30,     /* Σ  sequence     */
    T_RHO     = 39,     /* ρ  conjunction  */
    T_pi      = 38,     /* π  optional     */
    T_EPSILON = 34,     /* ε  null match   */
    T_LITERAL = 40,     /* σ  literal str  */
    T_ALPHA   = 32,     /* α  start of line */
    T_OMEGA   = 42,     /* ω  end of line  */
};

/* Actions */
#define PROCEED 0
#define SUCCESS 1
#define FAILURE 2
#define RECEDE  3

/*======================================================================================
 * C PATTERN node — runtime-built from Python tree
 *======================================================================================*/
#define MAX_CHILDREN 30

typedef struct Pattern {
    int                 type;
    int                 n;              /* child count (Σ,Π) or integer arg (POS,LEN) */
    const char *        s;              /* string arg (σ) — borrowed from Python       */
    Py_ssize_t          s_len;          /* length of s                                  */
    const char *        chars;          /* char set (ANY,SPAN,BREAK,NOTANY) — borrowed  */
    struct Pattern *    children[MAX_CHILDREN];
} Pattern;

/*======================================================================================
 * Arena — bump allocator, freed all at once after match
 *======================================================================================*/
typedef struct {
    Pattern *   nodes;
    int         count;
    int         capacity;
} Arena;

static Pattern *arena_alloc(Arena *a) {
    if (a->count >= a->capacity) {
        a->capacity = a->capacity ? a->capacity * 2 : 64;
        a->nodes = realloc(a->nodes, a->capacity * sizeof(Pattern));
        if (!a->nodes) return NULL;
    }
    Pattern *p = &a->nodes[a->count++];
    memset(p, 0, sizeof(Pattern));
    return p;
}

static void arena_free(Arena *a) {
    free(a->nodes);
    a->nodes = NULL;
    a->count = a->capacity = 0;
}

/*======================================================================================
 * Python → C PATTERN tree converter
 *
 * We identify Python classes by their __name__.  This is simple and works for
 * the UTF-8 Greek letters (σ, Σ, Π, ε, etc.) that the Python classes use.
 *======================================================================================*/

/* Helper: get class name as C string */
static const char *py_class_name(PyObject *obj) {
    PyObject *tp = (PyObject *)Py_TYPE(obj);
    PyObject *name = PyObject_GetAttrString(tp, "__name__");
    if (!name) return NULL;
    const char *s = PyUnicode_AsUTF8(name);
    Py_DECREF(name);
    return s;
}

/* Helper: get int attribute */
static int py_get_int(PyObject *obj, const char *attr) {
    PyObject *val = PyObject_GetAttrString(obj, attr);
    if (!val) return 0;
    int n = (int)PyLong_AsLong(val);
    Py_DECREF(val);
    return n;
}

/* Helper: get string attribute, return borrowed pointer + length */
static const char *py_get_str(PyObject *obj, const char *attr, Py_ssize_t *len) {
    PyObject *val = PyObject_GetAttrString(obj, attr);
    if (!val) return NULL;
    const char *s = PyUnicode_AsUTF8AndSize(val, len);
    Py_DECREF(val);
    return s;      /* safe: the Python PATTERN object keeps the string alive */
}

/* Forward decl */
static Pattern *convert(Arena *arena, PyObject *py_pat);

/* Convert children from a Python tuple (AP attribute) */
static int convert_children(Arena *arena, Pattern *p, PyObject *py_pat) {
    PyObject *AP = PyObject_GetAttrString(py_pat, "AP");
    if (!AP) return -1;
    Py_ssize_t n = PyTuple_Size(AP);
    if (n > MAX_CHILDREN) n = MAX_CHILDREN;
    p->n = (int)n;
    for (Py_ssize_t i = 0; i < n; i++) {
        PyObject *child = PyTuple_GetItem(AP, i);  /* borrowed */
        p->children[i] = convert(arena, child);
        if (!p->children[i]) { Py_DECREF(AP); return -1; }
    }
    Py_DECREF(AP);
    return 0;
}

/* Convert a single child from .P attribute */
static int convert_single_child(Arena *arena, Pattern *p, PyObject *py_pat) {
    PyObject *child = PyObject_GetAttrString(py_pat, "P");
    if (!child || child == Py_None) {
        Py_XDECREF(child);
        p->n = 0;
        return 0;
    }
    p->n = 1;
    p->children[0] = convert(arena, child);
    Py_DECREF(child);
    return p->children[0] ? 0 : -1;
}

/*
 * The main converter.  Maps Python class name → C type enum + attributes.
 *
 * Supports (Phase 1+):
 *   σ, POS, RPOS, LEN, TAB, RTAB, REM,
 *   ANY, NOTANY, SPAN, BREAK,
 *   Σ, Π, ρ, π, ε, α, ω,
 *   ARB, ARBNO, BAL, FENCE,
 *   FAIL, ABORT, SUCCEED
 */
static Pattern *convert(Arena *arena, PyObject *py_pat) {
    Pattern *p = arena_alloc(arena);
    if (!p) { PyErr_NoMemory(); return NULL; }

    const char *name = py_class_name(py_pat);
    if (!name) return NULL;

    /* --- Literal σ --- */
    if (strcmp(name, "\xcf\x83") == 0) {                 /* UTF-8 for σ */
        p->type = T_LITERAL;
        p->s = py_get_str(py_pat, "s", &p->s_len);
    }
    /* --- Position primitives with int .n --- */
    else if (strcmp(name, "POS") == 0) {
        p->type = T_POS;
        p->n = py_get_int(py_pat, "n");
    }
    else if (strcmp(name, "RPOS") == 0) {
        p->type = T_RPOS;
        p->n = py_get_int(py_pat, "n");
    }
    else if (strcmp(name, "LEN") == 0) {
        p->type = T_LEN;
        p->n = py_get_int(py_pat, "n");
    }
    else if (strcmp(name, "TAB") == 0) {
        p->type = T_TAB;
        p->n = py_get_int(py_pat, "n");
    }
    else if (strcmp(name, "RTAB") == 0) {
        p->type = T_RTAB;
        p->n = py_get_int(py_pat, "n");
    }
    /* --- Character set primitives with .chars --- */
    else if (strcmp(name, "ANY") == 0) {
        p->type = T_ANY;
        Py_ssize_t dummy;
        p->chars = py_get_str(py_pat, "chars", &dummy);
    }
    else if (strcmp(name, "NOTANY") == 0) {
        p->type = T_NOTANY;
        Py_ssize_t dummy;
        p->chars = py_get_str(py_pat, "chars", &dummy);
    }
    else if (strcmp(name, "SPAN") == 0) {
        p->type = T_SPAN;
        Py_ssize_t dummy;
        p->chars = py_get_str(py_pat, "chars", &dummy);
    }
    else if (strcmp(name, "BREAK") == 0) {
        p->type = T_BREAK;
        Py_ssize_t dummy;
        p->chars = py_get_str(py_pat, "chars", &dummy);
    }
    else if (strcmp(name, "BREAKX") == 0) {
        p->type = T_BREAK;  /* same as BREAK for now */
        Py_ssize_t dummy;
        p->chars = py_get_str(py_pat, "chars", &dummy);
    }
    /* --- Composites with children tuple .AP --- */
    else if (strcmp(name, "\xce\xa3") == 0) {            /* UTF-8 for Σ */
        p->type = T_SIGMA;
        if (convert_children(arena, p, py_pat) < 0) return NULL;
    }
    else if (strcmp(name, "\xce\xa0") == 0) {            /* UTF-8 for Π */
        p->type = T_PI;
        if (convert_children(arena, p, py_pat) < 0) return NULL;
    }
    else if (strcmp(name, "\xcf\x81") == 0) {            /* UTF-8 for ρ */
        p->type = T_RHO;
        if (convert_children(arena, p, py_pat) < 0) return NULL;
    }
    /* --- Single-child composites with .P --- */
    else if (strcmp(name, "\xcf\x80") == 0) {            /* UTF-8 for π */
        p->type = T_pi;
        if (convert_single_child(arena, p, py_pat) < 0) return NULL;
    }
    else if (strcmp(name, "ARBNO") == 0) {
        p->type = T_ARBNO;
        if (convert_single_child(arena, p, py_pat) < 0) return NULL;
    }
    else if (strcmp(name, "FENCE") == 0) {
        p->type = T_FENCE;
        if (convert_single_child(arena, p, py_pat) < 0) return NULL;
    }
    /* --- No-argument primitives --- */
    else if (strcmp(name, "\xce\xb5") == 0) {            /* UTF-8 for ε */
        p->type = T_EPSILON;
    }
    else if (strcmp(name, "\xce\xb1") == 0) {            /* UTF-8 for α */
        p->type = T_ALPHA;
    }
    else if (strcmp(name, "\xcf\x89") == 0) {            /* UTF-8 for ω */
        p->type = T_OMEGA;
    }
    else if (strcmp(name, "ARB") == 0) {
        p->type = T_ARB;
    }
    else if (strcmp(name, "BAL") == 0) {
        p->type = T_BAL;
    }
    else if (strcmp(name, "REM") == 0) {
        p->type = T_REM;
    }
    else if (strcmp(name, "FAIL") == 0) {
        p->type = T_FAIL;
    }
    else if (strcmp(name, "ABORT") == 0) {
        p->type = T_ABORT;
    }
    else if (strcmp(name, "SUCCEED") == 0) {
        p->type = T_SUCCEED;
    }
    else if (strcmp(name, "MARB") == 0) {
        p->type = T_MARB;
    }
    else {
        PyErr_Format(PyExc_TypeError,
            "snobol4c: unsupported pattern type '%s'", name);
        return NULL;
    }

    return p;
}

/*======================================================================================
 * Match engine state
 *======================================================================================*/

/*--- Psi: linked-list parent stack for tree descent (like original SNOBOL4c.c)
 *    Each node is separately allocated so omega save/restore gets
 *    a persistent snapshot — old entries are never overwritten.         ---*/
typedef struct PsiNode {
    Pattern *           PI;
    int                 ctx;
    struct PsiNode *    next;
} PsiNode;

static inline PsiNode *psi_push(PsiNode *psi, Pattern *PI, int ctx) {
    PsiNode *n = malloc(sizeof(PsiNode));
    n->PI   = PI;
    n->ctx  = ctx;
    n->next = psi;
    return n;
}

typedef struct {
    const char *    SIGMA;          /* scan-start pointer       */
    int             DELTA;          /* scan-start position      */
    int             OMEGA;          /* subject length           */
    const char *    sigma;          /* current pointer          */
    int             delta;          /* current position         */
    Pattern *       PI;             /* current pattern node     */
    int             fenced;
    int             yielded;
    int             ctx;            /* child index / counter    */
    PsiNode *       psi;            /* parent stack (linked list) */
} State;

/*--- Omega (Ω) backtrack stack ---*/
#define OMEGA_MAX 4096

static State    omega_stack[OMEGA_MAX];
static int      omega_top = -1;

static inline void omega_init(void)                     { omega_top = -1; }
static inline void omega_push(State *z)                 { assert(omega_top < OMEGA_MAX - 1); omega_stack[++omega_top] = *z; }
static inline int  omega_empty(void)                    { return omega_top < 0; }
static inline State *omega_tip(void)                    { return omega_top >= 0 ? &omega_stack[omega_top] : NULL; }
static inline void omega_pop(State *z) {
    if (omega_top >= 0) { *z = omega_stack[omega_top--]; }
    else                { memset(z, 0, sizeof(State)); z->PI = NULL; }
}

/*======================================================================================
 * State navigation (ζ functions from SNOBOL4c.c, using linked-list psi)
 *======================================================================================*/
static inline void z_down(State *z) {
    z->psi   = psi_push(z->psi, z->PI, z->ctx);
    z->sigma = z->SIGMA;
    z->delta = z->DELTA;
    z->PI    = z->PI->children[z->ctx];
    z->ctx   = 0;
}

static inline void z_down_single(State *z) {
    z->psi   = psi_push(z->psi, z->PI, z->ctx);
    z->sigma = z->SIGMA;
    z->delta = z->DELTA;
    z->PI    = z->PI->children[0];
    z->ctx   = 0;
}

static inline void z_up(State *z) {
    if (z->psi) { z->PI = z->psi->PI; z->ctx = z->psi->ctx; z->psi = z->psi->next; }
    else        { z->PI = NULL; }
}

static inline void z_up_track(State *z) {
    State *track = omega_tip();
    if (track) {
        track->SIGMA   = z->SIGMA;
        track->DELTA   = z->DELTA;
        track->sigma   = z->sigma;
        track->delta   = z->delta;
        track->yielded = 1;
    }
    z_up(z);
}

static inline void z_up_fail(State *z) {
    z_up(z);
}

static inline void z_next(State *z) {
    z->sigma = z->SIGMA;
    z->delta = z->DELTA;
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

/*======================================================================================
 * Pattern scanners (Π_ functions from SNOBOL4c.c)
 *======================================================================================*/
static inline bool scan_move(State *z, int delta) {
    if (delta >= 0 && z->DELTA + delta <= z->OMEGA) {
        z->sigma += delta;
        z->delta += delta;
        return true;
    }
    return false;      /* fixed: original was missing 'return' */
}

static bool scan_LITERAL(State *z) {
    const char *s = z->PI->s;
    Py_ssize_t len = z->PI->s_len;
    if (z->delta + len > z->OMEGA) return false;
    if (memcmp(z->sigma, s, len) != 0) return false;
    z->sigma += len;
    z->delta += len;
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
 *
 * Extracted from SNOBOL4c.c with:
 *   - psi as C stack (not heap)
 *   - no lambda/command stack
 *   - no globals dictionary
 *   - returns match result
 *======================================================================================*/
typedef struct {
    int matched;        /* 1 = success, 0 = failure */
    int start;          /* match start position     */
    int end;            /* match end position       */
} MatchResult;

static MatchResult engine_match(Pattern *pattern, const char *subject, int subject_len) {
    MatchResult result = {0, 0, 0};

    omega_init();

    int a = PROCEED;
    State Z;
    memset(&Z, 0, sizeof(Z));
    Z.SIGMA = subject;
    Z.DELTA = 0;
    Z.OMEGA = subject_len;
    Z.sigma = subject;
    Z.delta = 0;
    Z.PI    = pattern;
    Z.psi   = NULL;

    while (Z.PI) {
        int t = Z.PI->type;
        switch (t << 2 | a) {
/*--- Π (alternation) ---------------------------------------------------------------*/
        case T_PI<<2|PROCEED:
            if (Z.ctx < Z.PI->n) { a = PROCEED; omega_push(&Z);   z_down(&Z);        break; }
            else                 { a = RECEDE;   omega_pop(&Z);                        break; }
        case T_PI<<2|SUCCESS:    { a = SUCCESS;                    z_up(&Z);           break; }
        case T_PI<<2|FAILURE:    { a = PROCEED;                    z_stay_next(&Z);    break; }
        case T_PI<<2|RECEDE:
            if (!Z.fenced)       { a = PROCEED;                    z_stay_next(&Z);    break; }
            else                 { a = FAILURE;                    z_up_fail(&Z);      break; }
/*--- Σ (sequence) ------------------------------------------------------------------*/
        case T_SIGMA<<2|PROCEED:
            if (Z.ctx < Z.PI->n) { a = PROCEED;                   z_down(&Z);         break; }
            else                 { a = SUCCESS;                    z_up(&Z);           break; }
        case T_SIGMA<<2|SUCCESS: { a = PROCEED;                    z_move_next(&Z);    break; }
        case T_SIGMA<<2|FAILURE: { a = RECEDE;   omega_pop(&Z);                        break; }
/*--- ρ (conjunction) ---------------------------------------------------------------*/
        case T_RHO<<2|PROCEED:
            if (Z.ctx < Z.PI->n) { a = PROCEED;                   z_down(&Z);         break; }
            else                 { a = SUCCESS;                    z_up(&Z);           break; }
        case T_RHO<<2|SUCCESS:   { a = PROCEED;                    z_stay_next(&Z);    break; }
        case T_RHO<<2|FAILURE:   { a = RECEDE;   omega_pop(&Z);                        break; }
/*--- π (optional) ------------------------------------------------------------------*/
        case T_pi<<2|PROCEED:
            if (Z.ctx == 0)      { a = SUCCESS;  omega_push(&Z);  z_up(&Z);           break; }
            else if (Z.ctx == 1) { a = PROCEED;  omega_push(&Z);  z_down_single(&Z);  break; }
            else                 { a = RECEDE;   omega_pop(&Z);                        break; }
        case T_pi<<2|SUCCESS:    { a = SUCCESS;                    z_up(&Z);           break; }
        case T_pi<<2|FAILURE:    { a = FAILURE;                    z_up_fail(&Z);      break; }
        case T_pi<<2|RECEDE:
            if (!Z.fenced)       { a = PROCEED;                    z_stay_next(&Z);    break; }
            else                 { a = FAILURE;                    z_up_fail(&Z);      break; }
/*--- ARBNO -------------------------------------------------------------------------*/
        case T_ARBNO<<2|PROCEED:
            if (Z.ctx == 0)      { a = SUCCESS;  omega_push(&Z);  z_up_track(&Z);     break; }
            else                 { a = PROCEED;  omega_push(&Z);  z_down_single(&Z);  break; }
        case T_ARBNO<<2|SUCCESS: { a = SUCCESS;                    z_up_track(&Z);     break; }
        case T_ARBNO<<2|FAILURE: { a = RECEDE;   omega_pop(&Z);                        break; }
        case T_ARBNO<<2|RECEDE:
            if (Z.fenced)        { a = FAILURE;                    z_up_fail(&Z);      break; }
            else if (Z.yielded)  { a = PROCEED;                    z_move_next(&Z);    break; }
            else                 { a = FAILURE;                    z_up_fail(&Z);      break; }
/*--- ARB ---------------------------------------------------------------------------*/
        case T_ARB<<2|PROCEED:
            if (scan_ARB(&Z))    { a = SUCCESS;  omega_push(&Z);  z_up(&Z);           break; }
            else                 { a = RECEDE;   omega_pop(&Z);                        break; }
        case T_ARB<<2|RECEDE:
            if (!Z.fenced)       { a = PROCEED;                    z_stay_next(&Z);    break; }
            else                 { a = FAILURE;                    z_up_fail(&Z);      break; }
/*--- BAL ---------------------------------------------------------------------------*/
        case T_BAL<<2|PROCEED:
            if (scan_BAL(&Z))    { a = SUCCESS;  omega_push(&Z);  z_up(&Z);           break; }
            else                 { a = RECEDE;   omega_pop(&Z);                        break; }
        case T_BAL<<2|RECEDE:
            if (!Z.fenced)       { a = PROCEED;                    z_next(&Z);         break; }
            else                 { a = FAILURE;                    z_up_fail(&Z);      break; }
/*--- FENCE -------------------------------------------------------------------------*/
        case T_FENCE<<2|PROCEED:
            if (Z.PI->n == 0)    { a = SUCCESS;  omega_push(&Z);  z_up(&Z);           break; }
            else                 { a = PROCEED;  Z.fenced = 1;    z_down_single(&Z);  break; }
        case T_FENCE<<2|RECEDE:
            if (Z.PI->n == 0)    { a = RECEDE;                    Z.PI = NULL;        break; }
            else                 { assert(0); break; }
        case T_FENCE<<2|SUCCESS:
            if (Z.PI->n == 1)    { a = SUCCESS;  Z.fenced = 0;    z_up(&Z);           break; }
            else                 { assert(0); break; }
        case T_FENCE<<2|FAILURE:
            if (Z.PI->n == 1)    { a = FAILURE;  Z.fenced = 0;    z_up_fail(&Z);      break; }
            else                 { assert(0); break; }
/*--- Control primitives ------------------------------------------------------------*/
        case T_ABORT<<2|PROCEED:   { a = FAILURE;                  Z.PI = NULL;        break; }
        case T_SUCCEED<<2|PROCEED: { a = SUCCESS; omega_push(&Z);  z_up(&Z);           break; }
        case T_SUCCEED<<2|RECEDE:
            if (!Z.fenced)         { a = PROCEED;                  z_stay_next(&Z);    break; }
            else                   { a = FAILURE;                  z_up_fail(&Z);      break; }
        case T_FAIL<<2|PROCEED:    { a = FAILURE;                  z_up_fail(&Z);      break; }
        case T_EPSILON<<2|PROCEED: { a = SUCCESS;                  z_up(&Z);           break; }
/*--- Leaf scanners — only respond to PROCEED ---------------------------------------*/
        case T_LITERAL<<2|PROCEED:
            if (scan_LITERAL(&Z))  { a = SUCCESS; z_up(&Z);  break; }
            else                   { a = FAILURE; z_up_fail(&Z); break; }
        case T_ANY<<2|PROCEED:
            if (scan_ANY(&Z))      { a = SUCCESS; z_up(&Z);  break; }
            else                   { a = FAILURE; z_up_fail(&Z); break; }
        case T_NOTANY<<2|PROCEED:
            if (scan_NOTANY(&Z))   { a = SUCCESS; z_up(&Z);  break; }
            else                   { a = FAILURE; z_up_fail(&Z); break; }
        case T_SPAN<<2|PROCEED:
            if (scan_SPAN(&Z))     { a = SUCCESS; z_up(&Z);  break; }
            else                   { a = FAILURE; z_up_fail(&Z); break; }
        case T_BREAK<<2|PROCEED:
            if (scan_BREAK(&Z))    { a = SUCCESS; z_up(&Z);  break; }
            else                   { a = FAILURE; z_up_fail(&Z); break; }
        case T_POS<<2|PROCEED:
            if (scan_POS(&Z))      { a = SUCCESS; z_up(&Z);  break; }
            else                   { a = FAILURE; z_up_fail(&Z); break; }
        case T_RPOS<<2|PROCEED:
            if (scan_RPOS(&Z))     { a = SUCCESS; z_up(&Z);  break; }
            else                   { a = FAILURE; z_up_fail(&Z); break; }
        case T_LEN<<2|PROCEED:
            if (scan_LEN(&Z))      { a = SUCCESS; z_up(&Z);  break; }
            else                   { a = FAILURE; z_up_fail(&Z); break; }
        case T_TAB<<2|PROCEED:
            if (scan_TAB(&Z))      { a = SUCCESS; z_up(&Z);  break; }
            else                   { a = FAILURE; z_up_fail(&Z); break; }
        case T_RTAB<<2|PROCEED:
            if (scan_RTAB(&Z))     { a = SUCCESS; z_up(&Z);  break; }
            else                   { a = FAILURE; z_up_fail(&Z); break; }
        case T_REM<<2|PROCEED:
            if (scan_REM(&Z))      { a = SUCCESS; z_up(&Z);  break; }
            else                   { a = FAILURE; z_up_fail(&Z); break; }
        case T_ALPHA<<2|PROCEED:
            if (scan_ALPHA(&Z))    { a = SUCCESS; z_up(&Z);  break; }
            else                   { a = FAILURE; z_up_fail(&Z); break; }
        case T_OMEGA<<2|PROCEED:
            if (scan_OMEGA(&Z))    { a = SUCCESS; z_up(&Z);  break; }
            else                   { a = FAILURE; z_up_fail(&Z); break; }
/*--- MARB (like ARB) ---------------------------------------------------------------*/
        case T_MARB<<2|PROCEED:
            if (scan_ARB(&Z))      { a = SUCCESS; omega_push(&Z); z_up(&Z);  break; }
            else                   { a = RECEDE;  omega_pop(&Z);              break; }
        case T_MARB<<2|RECEDE:
            if (!Z.fenced)         { a = PROCEED;                  z_stay_next(&Z); break; }
            else                   { a = FAILURE;                  z_up_fail(&Z);   break; }
/*-----------------------------------------------------------------------------------*/
        default:
            /* Unhandled type/action combination — treat as failure */
            a = FAILURE;
            Z.PI = NULL;
            break;
        }
    }

    /* Determine result: success if the last action was SUCCESS when PI went NULL.
     * Use Z.delta (current scan cursor) not Z.DELTA (committed position) because
     * the final z_up exits before Σ can commit via z_move_next. */
    if (a == SUCCESS) {
        result.matched = 1;
        result.start   = 0;
        result.end     = Z.delta;
    }

    return result;
}

/*======================================================================================
 * CPython module: match(pattern, subject) → (start, end) or None
 *======================================================================================*/
static PyObject *py_match(PyObject *self, PyObject *args) {
    PyObject *py_pattern;
    const char *subject;
    Py_ssize_t subject_len;

    if (!PyArg_ParseTuple(args, "Os#", &py_pattern, &subject, &subject_len))
        return NULL;

    /* Build C pattern tree */
    Arena arena = {NULL, 0, 0};
    Pattern *root = convert(&arena, py_pattern);
    if (!root) {
        arena_free(&arena);
        return NULL;
    }

    /* Run engine */
    MatchResult result = engine_match(root, subject, (int)subject_len);

    /* Cleanup */
    arena_free(&arena);

    /* Return */
    if (result.matched)
        return Py_BuildValue("(ii)", result.start, result.end);
    Py_RETURN_NONE;
}

/*--- search(pattern, subject) → tries all starting positions -----------------------*/
static PyObject *py_search(PyObject *self, PyObject *args) {
    PyObject *py_pattern;
    const char *subject;
    Py_ssize_t subject_len;

    if (!PyArg_ParseTuple(args, "Os#", &py_pattern, &subject, &subject_len))
        return NULL;

    Arena arena = {NULL, 0, 0};
    Pattern *root = convert(&arena, py_pattern);
    if (!root) {
        arena_free(&arena);
        return NULL;
    }

    /* Try each starting position */
    for (Py_ssize_t start = 0; start <= subject_len; start++) {
        MatchResult result = engine_match(root, subject + start, (int)(subject_len - start));
        if (result.matched) {
            arena_free(&arena);
            return Py_BuildValue("(ii)", (int)start, (int)start + result.end);
        }
    }

    arena_free(&arena);
    Py_RETURN_NONE;
}

/*--- fullmatch(pattern, subject) → match requiring full consumption ----------------*/
static PyObject *py_fullmatch(PyObject *self, PyObject *args) {
    PyObject *py_pattern;
    const char *subject;
    Py_ssize_t subject_len;

    if (!PyArg_ParseTuple(args, "Os#", &py_pattern, &subject, &subject_len))
        return NULL;

    Arena arena = {NULL, 0, 0};
    Pattern *root = convert(&arena, py_pattern);
    if (!root) {
        arena_free(&arena);
        return NULL;
    }

    MatchResult result = engine_match(root, subject, (int)subject_len);

    arena_free(&arena);

    if (result.matched && result.end == (int)subject_len)
        return Py_BuildValue("(ii)", result.start, result.end);
    Py_RETURN_NONE;
}

/*======================================================================================
 * Module definition
 *======================================================================================*/
static PyMethodDef snobol4c_methods[] = {
    {"match",     py_match,     METH_VARARGS, "match(pattern, subject) -> (start, end) or None\n"
                                              "Anchored match at position 0."},
    {"search",    py_search,    METH_VARARGS, "search(pattern, subject) -> (start, end) or None\n"
                                              "Unanchored search across all starting positions."},
    {"fullmatch", py_fullmatch, METH_VARARGS, "fullmatch(pattern, subject) -> (start, end) or None\n"
                                              "Match requiring full subject consumption."},
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
