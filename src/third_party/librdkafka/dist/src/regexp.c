/**
 * Copyright: public domain
 *
 * From https://github.com/ccxvii/minilibs sha
 * 875c33568b5a4aa4fb3dd0c52ea98f7f0e5ca684:
 *
 * These libraries are in the public domain (or the equivalent where that is not
 * possible). You can do anything you want with them. You have no legal
 * obligation to do anything else, although I appreciate attribution.
 */

#include "rd.h"

#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <stdio.h>

#include "regexp.h"

#define nelem(a) (sizeof(a) / sizeof(a)[0])

typedef unsigned int Rune;

static int isalpharune(Rune c) {
        /* TODO: Add unicode support */
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

static Rune toupperrune(Rune c) {
        /* TODO: Add unicode support */
        if (c >= 'a' && c <= 'z')
                return c - 'a' + 'A';
        return c;
}

static int chartorune(Rune *r, const char *s) {
        /* TODO: Add UTF-8 decoding */
        *r = *s;
        return 1;
}

#define REPINF    255
#define MAXTHREAD 1000
#define MAXSUB    REG_MAXSUB

typedef struct Reclass Reclass;
typedef struct Renode Renode;
typedef struct Reinst Reinst;
typedef struct Rethread Rethread;
typedef struct Restate Restate;

struct Reclass {
        Rune *end;
        Rune spans[64];
};

struct Restate {
        Reprog *prog;
        Renode *pstart, *pend;

        const char *source;
        unsigned int ncclass;
        unsigned int nsub;
        Renode *sub[MAXSUB];

        int lookahead;
        Rune yychar;
        Reclass *yycc;
        int yymin, yymax;

        const char *error;
        jmp_buf kaboom;
};

struct Reprog {
        Reinst *start, *end;
        int flags;
        unsigned int nsub;
        Reclass cclass[16];
        Restate g; /**< Upstream has this as a global variable */
};

static void die(Restate *g, const char *message) {
        g->error = message;
        longjmp(g->kaboom, 1);
}

static Rune canon(Rune c) {
        Rune u = toupperrune(c);
        if (c >= 128 && u < 128)
                return c;
        return u;
}

/* Scan */

enum { L_CHAR = 256,
       L_CCLASS,  /* character class */
       L_NCCLASS, /* negative character class */
       L_NC,      /* "(?:" no capture */
       L_PLA,     /* "(?=" positive lookahead */
       L_NLA,     /* "(?!" negative lookahead */
       L_WORD,    /* "\b" word boundary */
       L_NWORD,   /* "\B" non-word boundary */
       L_REF,     /* "\1" back-reference */
       L_COUNT    /* {M,N} */
};

static int hex(Restate *g, int c) {
        if (c >= '0' && c <= '9')
                return c - '0';
        if (c >= 'a' && c <= 'f')
                return c - 'a' + 0xA;
        if (c >= 'A' && c <= 'F')
                return c - 'A' + 0xA;
        die(g, "invalid escape sequence");
        return 0;
}

static int dec(Restate *g, int c) {
        if (c >= '0' && c <= '9')
                return c - '0';
        die(g, "invalid quantifier");
        return 0;
}

#define ESCAPES "BbDdSsWw^$\\.*+?()[]{}|0123456789"

static int nextrune(Restate *g) {
        g->source += chartorune(&g->yychar, g->source);
        if (g->yychar == '\\') {
                g->source += chartorune(&g->yychar, g->source);
                switch (g->yychar) {
                case 0:
                        die(g, "unterminated escape sequence");
                case 'f':
                        g->yychar = '\f';
                        return 0;
                case 'n':
                        g->yychar = '\n';
                        return 0;
                case 'r':
                        g->yychar = '\r';
                        return 0;
                case 't':
                        g->yychar = '\t';
                        return 0;
                case 'v':
                        g->yychar = '\v';
                        return 0;
                case 'c':
                        g->yychar = (*g->source++) & 31;
                        return 0;
                case 'x':
                        g->yychar = hex(g, *g->source++) << 4;
                        g->yychar += hex(g, *g->source++);
                        if (g->yychar == 0) {
                                g->yychar = '0';
                                return 1;
                        }
                        return 0;
                case 'u':
                        g->yychar = hex(g, *g->source++) << 12;
                        g->yychar += hex(g, *g->source++) << 8;
                        g->yychar += hex(g, *g->source++) << 4;
                        g->yychar += hex(g, *g->source++);
                        if (g->yychar == 0) {
                                g->yychar = '0';
                                return 1;
                        }
                        return 0;
                }
                if (strchr(ESCAPES, g->yychar))
                        return 1;
                if (isalpharune(g->yychar) ||
                    g->yychar == '_') /* check identity escape */
                        die(g, "invalid escape character");
                return 0;
        }
        return 0;
}

static int lexcount(Restate *g) {
        g->yychar = *g->source++;

        g->yymin  = dec(g, g->yychar);
        g->yychar = *g->source++;
        while (g->yychar != ',' && g->yychar != '}') {
                g->yymin  = g->yymin * 10 + dec(g, g->yychar);
                g->yychar = *g->source++;
        }
        if (g->yymin >= REPINF)
                die(g, "numeric overflow");

        if (g->yychar == ',') {
                g->yychar = *g->source++;
                if (g->yychar == '}') {
                        g->yymax = REPINF;
                } else {
                        g->yymax  = dec(g, g->yychar);
                        g->yychar = *g->source++;
                        while (g->yychar != '}') {
                                g->yymax  = g->yymax * 10 + dec(g, g->yychar);
                                g->yychar = *g->source++;
                        }
                        if (g->yymax >= REPINF)
                                die(g, "numeric overflow");
                }
        } else {
                g->yymax = g->yymin;
        }

        return L_COUNT;
}

static void newcclass(Restate *g) {
        if (g->ncclass >= nelem(g->prog->cclass))
                die(g, "too many character classes");
        g->yycc      = g->prog->cclass + g->ncclass++;
        g->yycc->end = g->yycc->spans;
}

static void addrange(Restate *g, Rune a, Rune b) {
        if (a > b)
                die(g, "invalid character class range");
        if (g->yycc->end + 2 == g->yycc->spans + nelem(g->yycc->spans))
                die(g, "too many character class ranges");
        *g->yycc->end++ = a;
        *g->yycc->end++ = b;
}

static void addranges_d(Restate *g) {
        addrange(g, '0', '9');
}

static void addranges_D(Restate *g) {
        addrange(g, 0, '0' - 1);
        addrange(g, '9' + 1, 0xFFFF);
}

static void addranges_s(Restate *g) {
        addrange(g, 0x9, 0x9);
        addrange(g, 0xA, 0xD);
        addrange(g, 0x20, 0x20);
        addrange(g, 0xA0, 0xA0);
        addrange(g, 0x2028, 0x2029);
        addrange(g, 0xFEFF, 0xFEFF);
}

static void addranges_S(Restate *g) {
        addrange(g, 0, 0x9 - 1);
        addrange(g, 0x9 + 1, 0xA - 1);
        addrange(g, 0xD + 1, 0x20 - 1);
        addrange(g, 0x20 + 1, 0xA0 - 1);
        addrange(g, 0xA0 + 1, 0x2028 - 1);
        addrange(g, 0x2029 + 1, 0xFEFF - 1);
        addrange(g, 0xFEFF + 1, 0xFFFF);
}

static void addranges_w(Restate *g) {
        addrange(g, '0', '9');
        addrange(g, 'A', 'Z');
        addrange(g, '_', '_');
        addrange(g, 'a', 'z');
}

static void addranges_W(Restate *g) {
        addrange(g, 0, '0' - 1);
        addrange(g, '9' + 1, 'A' - 1);
        addrange(g, 'Z' + 1, '_' - 1);
        addrange(g, '_' + 1, 'a' - 1);
        addrange(g, 'z' + 1, 0xFFFF);
}

static int lexclass(Restate *g) {
        int type = L_CCLASS;
        int quoted, havesave, havedash;
        Rune save = 0;

        newcclass(g);

        quoted = nextrune(g);
        if (!quoted && g->yychar == '^') {
                type   = L_NCCLASS;
                quoted = nextrune(g);
        }

        havesave = havedash = 0;
        for (;;) {
                if (g->yychar == 0)
                        die(g, "unterminated character class");
                if (!quoted && g->yychar == ']')
                        break;

                if (!quoted && g->yychar == '-') {
                        if (havesave) {
                                if (havedash) {
                                        addrange(g, save, '-');
                                        havesave = havedash = 0;
                                } else {
                                        havedash = 1;
                                }
                        } else {
                                save     = '-';
                                havesave = 1;
                        }
                } else if (quoted && strchr("DSWdsw", g->yychar)) {
                        if (havesave) {
                                addrange(g, save, save);
                                if (havedash)
                                        addrange(g, '-', '-');
                        }
                        switch (g->yychar) {
                        case 'd':
                                addranges_d(g);
                                break;
                        case 's':
                                addranges_s(g);
                                break;
                        case 'w':
                                addranges_w(g);
                                break;
                        case 'D':
                                addranges_D(g);
                                break;
                        case 'S':
                                addranges_S(g);
                                break;
                        case 'W':
                                addranges_W(g);
                                break;
                        }
                        havesave = havedash = 0;
                } else {
                        if (quoted) {
                                if (g->yychar == 'b')
                                        g->yychar = '\b';
                                else if (g->yychar == '0')
                                        g->yychar = 0;
                                /* else identity escape */
                        }
                        if (havesave) {
                                if (havedash) {
                                        addrange(g, save, g->yychar);
                                        havesave = havedash = 0;
                                } else {
                                        addrange(g, save, save);
                                        save = g->yychar;
                                }
                        } else {
                                save     = g->yychar;
                                havesave = 1;
                        }
                }

                quoted = nextrune(g);
        }

        if (havesave) {
                addrange(g, save, save);
                if (havedash)
                        addrange(g, '-', '-');
        }

        return type;
}

static int lex(Restate *g) {
        int quoted = nextrune(g);
        if (quoted) {
                switch (g->yychar) {
                case 'b':
                        return L_WORD;
                case 'B':
                        return L_NWORD;
                case 'd':
                        newcclass(g);
                        addranges_d(g);
                        return L_CCLASS;
                case 's':
                        newcclass(g);
                        addranges_s(g);
                        return L_CCLASS;
                case 'w':
                        newcclass(g);
                        addranges_w(g);
                        return L_CCLASS;
                case 'D':
                        newcclass(g);
                        addranges_d(g);
                        return L_NCCLASS;
                case 'S':
                        newcclass(g);
                        addranges_s(g);
                        return L_NCCLASS;
                case 'W':
                        newcclass(g);
                        addranges_w(g);
                        return L_NCCLASS;
                case '0':
                        g->yychar = 0;
                        return L_CHAR;
                }
                if (g->yychar >= '0' && g->yychar <= '9') {
                        g->yychar -= '0';
                        if (*g->source >= '0' && *g->source <= '9')
                                g->yychar = g->yychar * 10 + *g->source++ - '0';
                        return L_REF;
                }
                return L_CHAR;
        }

        switch (g->yychar) {
        case 0:
        case '$':
        case ')':
        case '*':
        case '+':
        case '.':
        case '?':
        case '^':
        case '|':
                return g->yychar;
        }

        if (g->yychar == '{')
                return lexcount(g);
        if (g->yychar == '[')
                return lexclass(g);
        if (g->yychar == '(') {
                if (g->source[0] == '?') {
                        if (g->source[1] == ':') {
                                g->source += 2;
                                return L_NC;
                        }
                        if (g->source[1] == '=') {
                                g->source += 2;
                                return L_PLA;
                        }
                        if (g->source[1] == '!') {
                                g->source += 2;
                                return L_NLA;
                        }
                }
                return '(';
        }

        return L_CHAR;
}

/* Parse */

enum { P_CAT,
       P_ALT,
       P_REP,
       P_BOL,
       P_EOL,
       P_WORD,
       P_NWORD,
       P_PAR,
       P_PLA,
       P_NLA,
       P_ANY,
       P_CHAR,
       P_CCLASS,
       P_NCCLASS,
       P_REF };

struct Renode {
        unsigned char type;
        unsigned char ng, m, n;
        Rune c;
        Reclass *cc;
        Renode *x;
        Renode *y;
};

static Renode *newnode(Restate *g, int type) {
        Renode *node = g->pend++;
        node->type   = type;
        node->cc     = NULL;
        node->c      = 0;
        node->ng     = 0;
        node->m      = 0;
        node->n      = 0;
        node->x = node->y = NULL;
        return node;
}

static int empty(Renode *node) {
        if (!node)
                return 1;
        switch (node->type) {
        default:
                return 1;
        case P_CAT:
                return empty(node->x) && empty(node->y);
        case P_ALT:
                return empty(node->x) || empty(node->y);
        case P_REP:
                return empty(node->x) || node->m == 0;
        case P_PAR:
                return empty(node->x);
        case P_REF:
                return empty(node->x);
        case P_ANY:
        case P_CHAR:
        case P_CCLASS:
        case P_NCCLASS:
                return 0;
        }
}

static Renode *newrep(Restate *g, Renode *atom, int ng, int min, int max) {
        Renode *rep = newnode(g, P_REP);
        if (max == REPINF && empty(atom))
                die(g, "infinite loop matching the empty string");
        rep->ng = ng;
        rep->m  = min;
        rep->n  = max;
        rep->x  = atom;
        return rep;
}

static void next(Restate *g) {
        g->lookahead = lex(g);
}

static int re_accept(Restate *g, int t) {
        if (g->lookahead == t) {
                next(g);
                return 1;
        }
        return 0;
}

static Renode *parsealt(Restate *g);

static Renode *parseatom(Restate *g) {
        Renode *atom;
        if (g->lookahead == L_CHAR) {
                atom    = newnode(g, P_CHAR);
                atom->c = g->yychar;
                next(g);
                return atom;
        }
        if (g->lookahead == L_CCLASS) {
                atom     = newnode(g, P_CCLASS);
                atom->cc = g->yycc;
                next(g);
                return atom;
        }
        if (g->lookahead == L_NCCLASS) {
                atom     = newnode(g, P_NCCLASS);
                atom->cc = g->yycc;
                next(g);
                return atom;
        }
        if (g->lookahead == L_REF) {
                atom = newnode(g, P_REF);
                if (g->yychar == 0 || g->yychar > g->nsub || !g->sub[g->yychar])
                        die(g, "invalid back-reference");
                atom->n = g->yychar;
                atom->x = g->sub[g->yychar];
                next(g);
                return atom;
        }
        if (re_accept(g, '.'))
                return newnode(g, P_ANY);
        if (re_accept(g, '(')) {
                atom = newnode(g, P_PAR);
                if (g->nsub == MAXSUB)
                        die(g, "too many captures");
                atom->n         = g->nsub++;
                atom->x         = parsealt(g);
                g->sub[atom->n] = atom;
                if (!re_accept(g, ')'))
                        die(g, "unmatched '('");
                return atom;
        }
        if (re_accept(g, L_NC)) {
                atom = parsealt(g);
                if (!re_accept(g, ')'))
                        die(g, "unmatched '('");
                return atom;
        }
        if (re_accept(g, L_PLA)) {
                atom    = newnode(g, P_PLA);
                atom->x = parsealt(g);
                if (!re_accept(g, ')'))
                        die(g, "unmatched '('");
                return atom;
        }
        if (re_accept(g, L_NLA)) {
                atom    = newnode(g, P_NLA);
                atom->x = parsealt(g);
                if (!re_accept(g, ')'))
                        die(g, "unmatched '('");
                return atom;
        }
        die(g, "syntax error");
        return NULL;
}

static Renode *parserep(Restate *g) {
        Renode *atom;

        if (re_accept(g, '^'))
                return newnode(g, P_BOL);
        if (re_accept(g, '$'))
                return newnode(g, P_EOL);
        if (re_accept(g, L_WORD))
                return newnode(g, P_WORD);
        if (re_accept(g, L_NWORD))
                return newnode(g, P_NWORD);

        atom = parseatom(g);
        if (g->lookahead == L_COUNT) {
                int min = g->yymin, max = g->yymax;
                next(g);
                if (max < min)
                        die(g, "invalid quantifier");
                return newrep(g, atom, re_accept(g, '?'), min, max);
        }
        if (re_accept(g, '*'))
                return newrep(g, atom, re_accept(g, '?'), 0, REPINF);
        if (re_accept(g, '+'))
                return newrep(g, atom, re_accept(g, '?'), 1, REPINF);
        if (re_accept(g, '?'))
                return newrep(g, atom, re_accept(g, '?'), 0, 1);
        return atom;
}

static Renode *parsecat(Restate *g) {
        Renode *cat, *x;
        if (g->lookahead && g->lookahead != '|' && g->lookahead != ')') {
                cat = parserep(g);
                while (g->lookahead && g->lookahead != '|' &&
                       g->lookahead != ')') {
                        x      = cat;
                        cat    = newnode(g, P_CAT);
                        cat->x = x;
                        cat->y = parserep(g);
                }
                return cat;
        }
        return NULL;
}

static Renode *parsealt(Restate *g) {
        Renode *alt, *x;
        alt = parsecat(g);
        while (re_accept(g, '|')) {
                x      = alt;
                alt    = newnode(g, P_ALT);
                alt->x = x;
                alt->y = parsecat(g);
        }
        return alt;
}

/* Compile */

enum { I_END,
       I_JUMP,
       I_SPLIT,
       I_PLA,
       I_NLA,
       I_ANYNL,
       I_ANY,
       I_CHAR,
       I_CCLASS,
       I_NCCLASS,
       I_REF,
       I_BOL,
       I_EOL,
       I_WORD,
       I_NWORD,
       I_LPAR,
       I_RPAR };

struct Reinst {
        unsigned char opcode;
        unsigned char n;
        Rune c;
        Reclass *cc;
        Reinst *x;
        Reinst *y;
};

static unsigned int count(Renode *node) {
        unsigned int min, max;
        if (!node)
                return 0;
        switch (node->type) {
        default:
                return 1;
        case P_CAT:
                return count(node->x) + count(node->y);
        case P_ALT:
                return count(node->x) + count(node->y) + 2;
        case P_REP:
                min = node->m;
                max = node->n;
                if (min == max)
                        return count(node->x) * min;
                if (max < REPINF)
                        return count(node->x) * max + (max - min);
                return count(node->x) * (min + 1) + 2;
        case P_PAR:
                return count(node->x) + 2;
        case P_PLA:
                return count(node->x) + 2;
        case P_NLA:
                return count(node->x) + 2;
        }
}

static Reinst *emit(Reprog *prog, int opcode) {
        Reinst *inst = prog->end++;
        inst->opcode = opcode;
        inst->n      = 0;
        inst->c      = 0;
        inst->cc     = NULL;
        inst->x = inst->y = NULL;
        return inst;
}

static void compile(Reprog *prog, Renode *node) {
        Reinst *inst, *split, *jump;
        unsigned int i;

        if (!node)
                return;

        switch (node->type) {
        case P_CAT:
                compile(prog, node->x);
                compile(prog, node->y);
                break;

        case P_ALT:
                split = emit(prog, I_SPLIT);
                compile(prog, node->x);
                jump = emit(prog, I_JUMP);
                compile(prog, node->y);
                split->x = split + 1;
                split->y = jump + 1;
                jump->x  = prog->end;
                break;

        case P_REP:
                for (i = 0; i < node->m; ++i) {
                        inst = prog->end;
                        compile(prog, node->x);
                }
                if (node->m == node->n)
                        break;
                if (node->n < REPINF) {
                        for (i = node->m; i < node->n; ++i) {
                                split = emit(prog, I_SPLIT);
                                compile(prog, node->x);
                                if (node->ng) {
                                        split->y = split + 1;
                                        split->x = prog->end;
                                } else {
                                        split->x = split + 1;
                                        split->y = prog->end;
                                }
                        }
                } else if (node->m == 0) {
                        split = emit(prog, I_SPLIT);
                        compile(prog, node->x);
                        jump = emit(prog, I_JUMP);
                        if (node->ng) {
                                split->y = split + 1;
                                split->x = prog->end;
                        } else {
                                split->x = split + 1;
                                split->y = prog->end;
                        }
                        jump->x = split;
                } else {
                        split = emit(prog, I_SPLIT);
                        if (node->ng) {
                                split->y = inst;
                                split->x = prog->end;
                        } else {
                                split->x = inst;
                                split->y = prog->end;
                        }
                }
                break;

        case P_BOL:
                emit(prog, I_BOL);
                break;
        case P_EOL:
                emit(prog, I_EOL);
                break;
        case P_WORD:
                emit(prog, I_WORD);
                break;
        case P_NWORD:
                emit(prog, I_NWORD);
                break;

        case P_PAR:
                inst    = emit(prog, I_LPAR);
                inst->n = node->n;
                compile(prog, node->x);
                inst    = emit(prog, I_RPAR);
                inst->n = node->n;
                break;
        case P_PLA:
                split = emit(prog, I_PLA);
                compile(prog, node->x);
                emit(prog, I_END);
                split->x = split + 1;
                split->y = prog->end;
                break;
        case P_NLA:
                split = emit(prog, I_NLA);
                compile(prog, node->x);
                emit(prog, I_END);
                split->x = split + 1;
                split->y = prog->end;
                break;

        case P_ANY:
                emit(prog, I_ANY);
                break;
        case P_CHAR:
                inst    = emit(prog, I_CHAR);
                inst->c = (prog->flags & REG_ICASE) ? canon(node->c) : node->c;
                break;
        case P_CCLASS:
                inst     = emit(prog, I_CCLASS);
                inst->cc = node->cc;
                break;
        case P_NCCLASS:
                inst     = emit(prog, I_NCCLASS);
                inst->cc = node->cc;
                break;
        case P_REF:
                inst    = emit(prog, I_REF);
                inst->n = node->n;
                break;
        }
}

#ifdef TEST
static void dumpnode(Renode *node) {
        Rune *p;
        if (!node) {
                printf("Empty");
                return;
        }
        switch (node->type) {
        case P_CAT:
                printf("Cat(");
                dumpnode(node->x);
                printf(", ");
                dumpnode(node->y);
                printf(")");
                break;
        case P_ALT:
                printf("Alt(");
                dumpnode(node->x);
                printf(", ");
                dumpnode(node->y);
                printf(")");
                break;
        case P_REP:
                printf(node->ng ? "NgRep(%d,%d," : "Rep(%d,%d,", node->m,
                       node->n);
                dumpnode(node->x);
                printf(")");
                break;
        case P_BOL:
                printf("Bol");
                break;
        case P_EOL:
                printf("Eol");
                break;
        case P_WORD:
                printf("Word");
                break;
        case P_NWORD:
                printf("NotWord");
                break;
        case P_PAR:
                printf("Par(%d,", node->n);
                dumpnode(node->x);
                printf(")");
                break;
        case P_PLA:
                printf("PLA(");
                dumpnode(node->x);
                printf(")");
                break;
        case P_NLA:
                printf("NLA(");
                dumpnode(node->x);
                printf(")");
                break;
        case P_ANY:
                printf("Any");
                break;
        case P_CHAR:
                printf("Char(%c)", node->c);
                break;
        case P_CCLASS:
                printf("Class(");
                for (p = node->cc->spans; p < node->cc->end; p += 2)
                        printf("%02X-%02X,", p[0], p[1]);
                printf(")");
                break;
        case P_NCCLASS:
                printf("NotClass(");
                for (p = node->cc->spans; p < node->cc->end; p += 2)
                        printf("%02X-%02X,", p[0], p[1]);
                printf(")");
                break;
        case P_REF:
                printf("Ref(%d)", node->n);
                break;
        }
}

static void dumpprog(Reprog *prog) {
        Reinst *inst;
        int i;
        for (i = 0, inst = prog->start; inst < prog->end; ++i, ++inst) {
                printf("% 5d: ", i);
                switch (inst->opcode) {
                case I_END:
                        puts("end");
                        break;
                case I_JUMP:
                        printf("jump %d\n", (int)(inst->x - prog->start));
                        break;
                case I_SPLIT:
                        printf("split %d %d\n", (int)(inst->x - prog->start),
                               (int)(inst->y - prog->start));
                        break;
                case I_PLA:
                        printf("pla %d %d\n", (int)(inst->x - prog->start),
                               (int)(inst->y - prog->start));
                        break;
                case I_NLA:
                        printf("nla %d %d\n", (int)(inst->x - prog->start),
                               (int)(inst->y - prog->start));
                        break;
                case I_ANY:
                        puts("any");
                        break;
                case I_ANYNL:
                        puts("anynl");
                        break;
                case I_CHAR:
                        printf(inst->c >= 32 && inst->c < 127 ? "char '%c'\n"
                                                              : "char U+%04X\n",
                               inst->c);
                        break;
                case I_CCLASS:
                        puts("cclass");
                        break;
                case I_NCCLASS:
                        puts("ncclass");
                        break;
                case I_REF:
                        printf("ref %d\n", inst->n);
                        break;
                case I_BOL:
                        puts("bol");
                        break;
                case I_EOL:
                        puts("eol");
                        break;
                case I_WORD:
                        puts("word");
                        break;
                case I_NWORD:
                        puts("nword");
                        break;
                case I_LPAR:
                        printf("lpar %d\n", inst->n);
                        break;
                case I_RPAR:
                        printf("rpar %d\n", inst->n);
                        break;
                }
        }
}
#endif

Reprog *re_regcomp(const char *pattern, int cflags, const char **errorp) {
        Reprog *prog;
        Restate *g;
        Renode *node;
        Reinst *split, *jump;
        int i;
        unsigned int ncount;
        size_t pattern_len = strlen(pattern);

        if (pattern_len > 10000) {
                /* Avoid stack exhaustion in recursive parseatom() et.al. */
                if (errorp)
                        *errorp = "regexp pattern too long (max 10000)";
                return NULL;
        }

        prog      = rd_calloc(1, sizeof(Reprog));
        g         = &prog->g;
        g->prog   = prog;
        g->pstart = g->pend = rd_malloc(sizeof(Renode) * pattern_len * 2);

        if (setjmp(g->kaboom)) {
                if (errorp)
                        *errorp = g->error;
                rd_free(g->pstart);
                rd_free(prog);
                return NULL;
        }

        g->source  = pattern;
        g->ncclass = 0;
        g->nsub    = 1;
        for (i = 0; i < MAXSUB; ++i)
                g->sub[i] = 0;

        g->prog->flags = cflags;

        next(g);
        node = parsealt(g);
        if (g->lookahead == ')')
                die(g, "unmatched ')'");
        if (g->lookahead != 0)
                die(g, "syntax error");

        g->prog->nsub = g->nsub;
        ncount        = count(node);
        if (ncount > 10000)
                die(g, "regexp graph too large");
        g->prog->start = g->prog->end =
            rd_malloc((ncount + 6) * sizeof(Reinst));

        split    = emit(g->prog, I_SPLIT);
        split->x = split + 3;
        split->y = split + 1;
        emit(g->prog, I_ANYNL);
        jump    = emit(g->prog, I_JUMP);
        jump->x = split;
        emit(g->prog, I_LPAR);
        compile(g->prog, node);
        emit(g->prog, I_RPAR);
        emit(g->prog, I_END);

#ifdef TEST
        dumpnode(node);
        putchar('\n');
        dumpprog(g->prog);
#endif

        rd_free(g->pstart);

        if (errorp)
                *errorp = NULL;
        return g->prog;
}

void re_regfree(Reprog *prog) {
        if (prog) {
                rd_free(prog->start);
                rd_free(prog);
        }
}

/* Match */

static int isnewline(int c) {
        return c == 0xA || c == 0xD || c == 0x2028 || c == 0x2029;
}

static int iswordchar(int c) {
        return c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9');
}

static int incclass(Reclass *cc, Rune c) {
        Rune *p;
        for (p = cc->spans; p < cc->end; p += 2)
                if (p[0] <= c && c <= p[1])
                        return 1;
        return 0;
}

static int incclasscanon(Reclass *cc, Rune c) {
        Rune *p, r;
        for (p = cc->spans; p < cc->end; p += 2)
                for (r = p[0]; r <= p[1]; ++r)
                        if (c == canon(r))
                                return 1;
        return 0;
}

static int strncmpcanon(const char *a, const char *b, unsigned int n) {
        Rune ra, rb;
        int c;
        while (n--) {
                if (!*a)
                        return -1;
                if (!*b)
                        return 1;
                a += chartorune(&ra, a);
                b += chartorune(&rb, b);
                c = canon(ra) - canon(rb);
                if (c)
                        return c;
        }
        return 0;
}

struct Rethread {
        Reinst *pc;
        const char *sp;
        Resub sub;
};

static void spawn(Rethread *t, Reinst *pc, const char *sp, Resub *sub) {
        t->pc = pc;
        t->sp = sp;
        memcpy(&t->sub, sub, sizeof t->sub);
}

static int
match(Reinst *pc, const char *sp, const char *bol, int flags, Resub *out) {
        Rethread ready[MAXTHREAD];
        Resub scratch;
        Resub sub;
        Rune c;
        unsigned int nready;
        int i;

        /* queue initial thread */
        spawn(ready + 0, pc, sp, out);
        nready = 1;

        /* run threads in stack order */
        while (nready > 0) {
                --nready;
                pc = ready[nready].pc;
                sp = ready[nready].sp;
                memcpy(&sub, &ready[nready].sub, sizeof sub);
                for (;;) {
                        switch (pc->opcode) {
                        case I_END:
                                for (i = 0; i < MAXSUB; ++i) {
                                        out->sub[i].sp = sub.sub[i].sp;
                                        out->sub[i].ep = sub.sub[i].ep;
                                }
                                return 1;
                        case I_JUMP:
                                pc = pc->x;
                                continue;
                        case I_SPLIT:
                                if (nready >= MAXTHREAD) {
                                        fprintf(
                                            stderr,
                                            "regexec: backtrack overflow!\n");
                                        return 0;
                                }
                                spawn(&ready[nready++], pc->y, sp, &sub);
                                pc = pc->x;
                                continue;

                        case I_PLA:
                                if (!match(pc->x, sp, bol, flags, &sub))
                                        goto dead;
                                pc = pc->y;
                                continue;
                        case I_NLA:
                                memcpy(&scratch, &sub, sizeof scratch);
                                if (match(pc->x, sp, bol, flags, &scratch))
                                        goto dead;
                                pc = pc->y;
                                continue;

                        case I_ANYNL:
                                sp += chartorune(&c, sp);
                                if (c == 0)
                                        goto dead;
                                break;
                        case I_ANY:
                                sp += chartorune(&c, sp);
                                if (c == 0)
                                        goto dead;
                                if (isnewline(c))
                                        goto dead;
                                break;
                        case I_CHAR:
                                sp += chartorune(&c, sp);
                                if (c == 0)
                                        goto dead;
                                if (flags & REG_ICASE)
                                        c = canon(c);
                                if (c != pc->c)
                                        goto dead;
                                break;
                        case I_CCLASS:
                                sp += chartorune(&c, sp);
                                if (c == 0)
                                        goto dead;
                                if (flags & REG_ICASE) {
                                        if (!incclasscanon(pc->cc, canon(c)))
                                                goto dead;
                                } else {
                                        if (!incclass(pc->cc, c))
                                                goto dead;
                                }
                                break;
                        case I_NCCLASS:
                                sp += chartorune(&c, sp);
                                if (c == 0)
                                        goto dead;
                                if (flags & REG_ICASE) {
                                        if (incclasscanon(pc->cc, canon(c)))
                                                goto dead;
                                } else {
                                        if (incclass(pc->cc, c))
                                                goto dead;
                                }
                                break;
                        case I_REF:
                                i = (int)(sub.sub[pc->n].ep -
                                          sub.sub[pc->n].sp);
                                if (flags & REG_ICASE) {
                                        if (strncmpcanon(sp, sub.sub[pc->n].sp,
                                                         i))
                                                goto dead;
                                } else {
                                        if (strncmp(sp, sub.sub[pc->n].sp, i))
                                                goto dead;
                                }
                                if (i > 0)
                                        sp += i;
                                break;

                        case I_BOL:
                                if (sp == bol && !(flags & REG_NOTBOL))
                                        break;
                                if (flags & REG_NEWLINE)
                                        if (sp > bol && isnewline(sp[-1]))
                                                break;
                                goto dead;
                        case I_EOL:
                                if (*sp == 0)
                                        break;
                                if (flags & REG_NEWLINE)
                                        if (isnewline(*sp))
                                                break;
                                goto dead;
                        case I_WORD:
                                i = sp > bol && iswordchar(sp[-1]);
                                i ^= iswordchar(sp[0]);
                                if (i)
                                        break;
                                goto dead;
                        case I_NWORD:
                                i = sp > bol && iswordchar(sp[-1]);
                                i ^= iswordchar(sp[0]);
                                if (!i)
                                        break;
                                goto dead;

                        case I_LPAR:
                                sub.sub[pc->n].sp = sp;
                                break;
                        case I_RPAR:
                                sub.sub[pc->n].ep = sp;
                                break;
                        default:
                                goto dead;
                        }
                        pc = pc + 1;
                }
        dead:;
        }
        return 0;
}

int re_regexec(Reprog *prog, const char *sp, Resub *sub, int eflags) {
        Resub scratch;
        int i;

        if (!sub)
                sub = &scratch;

        sub->nsub = prog->nsub;
        for (i = 0; i < MAXSUB; ++i)
                sub->sub[i].sp = sub->sub[i].ep = NULL;

        return !match(prog->start, sp, sp, prog->flags | eflags, sub);
}

#ifdef TEST
int main(int argc, char **argv) {
        const char *error;
        const char *s;
        Reprog *p;
        Resub m;
        unsigned int i;

        if (argc > 1) {
                p = regcomp(argv[1], 0, &error);
                if (!p) {
                        fprintf(stderr, "regcomp: %s\n", error);
                        return 1;
                }

                if (argc > 2) {
                        s = argv[2];
                        printf("nsub = %d\n", p->nsub);
                        if (!regexec(p, s, &m, 0)) {
                                for (i = 0; i < m.nsub; ++i) {
                                        int n = m.sub[i].ep - m.sub[i].sp;
                                        if (n > 0)
                                                printf(
                                                    "match %d: s=%d e=%d n=%d "
                                                    "'%.*s'\n",
                                                    i, (int)(m.sub[i].sp - s),
                                                    (int)(m.sub[i].ep - s), n,
                                                    n, m.sub[i].sp);
                                        else
                                                printf("match %d: n=0 ''\n", i);
                                }
                        } else {
                                printf("no match\n");
                        }
                }
        }

        return 0;
}
#endif
