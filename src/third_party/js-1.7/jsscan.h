/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 *
 * ***** BEGIN LICENSE BLOCK *****
 * Version: MPL 1.1/GPL 2.0/LGPL 2.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is Mozilla Communicator client code, released
 * March 31, 1998.
 *
 * The Initial Developer of the Original Code is
 * Netscape Communications Corporation.
 * Portions created by the Initial Developer are Copyright (C) 1998
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 *
 * Alternatively, the contents of this file may be used under the terms of
 * either of the GNU General Public License Version 2 or later (the "GPL"),
 * or the GNU Lesser General Public License Version 2.1 or later (the "LGPL"),
 * in which case the provisions of the GPL or the LGPL are applicable instead
 * of those above. If you wish to allow use of your version of this file only
 * under the terms of either the GPL or the LGPL, and not to allow others to
 * use your version of this file under the terms of the MPL, indicate your
 * decision by deleting the provisions above and replace them with the notice
 * and other provisions required by the GPL or the LGPL. If you do not delete
 * the provisions above, a recipient may use your version of this file under
 * the terms of any one of the MPL, the GPL or the LGPL.
 *
 * ***** END LICENSE BLOCK ***** */

#ifndef jsscan_h___
#define jsscan_h___
/*
 * JS lexical scanner interface.
 */
#include <stddef.h>
#include <stdio.h>
#include "jsconfig.h"
#include "jsopcode.h"
#include "jsprvtd.h"
#include "jspubtd.h"

JS_BEGIN_EXTERN_C

#define JS_KEYWORD(keyword, type, op, version) \
    extern const char js_##keyword##_str[];
#include "jskeyword.tbl"
#undef JS_KEYWORD

typedef enum JSTokenType {
    TOK_ERROR = -1,                     /* well-known as the only code < EOF */
    TOK_EOF = 0,                        /* end of file */
    TOK_EOL = 1,                        /* end of line */
    TOK_SEMI = 2,                       /* semicolon */
    TOK_COMMA = 3,                      /* comma operator */
    TOK_ASSIGN = 4,                     /* assignment ops (= += -= etc.) */
    TOK_HOOK = 5, TOK_COLON = 6,        /* conditional (?:) */
    TOK_OR = 7,                         /* logical or (||) */
    TOK_AND = 8,                        /* logical and (&&) */
    TOK_BITOR = 9,                      /* bitwise-or (|) */
    TOK_BITXOR = 10,                    /* bitwise-xor (^) */
    TOK_BITAND = 11,                    /* bitwise-and (&) */
    TOK_EQOP = 12,                      /* equality ops (== !=) */
    TOK_RELOP = 13,                     /* relational ops (< <= > >=) */
    TOK_SHOP = 14,                      /* shift ops (<< >> >>>) */
    TOK_PLUS = 15,                      /* plus */
    TOK_MINUS = 16,                     /* minus */
    TOK_STAR = 17, TOK_DIVOP = 18,      /* multiply/divide ops (* / %) */
    TOK_UNARYOP = 19,                   /* unary prefix operator */
    TOK_INC = 20, TOK_DEC = 21,         /* increment/decrement (++ --) */
    TOK_DOT = 22,                       /* member operator (.) */
    TOK_LB = 23, TOK_RB = 24,           /* left and right brackets */
    TOK_LC = 25, TOK_RC = 26,           /* left and right curlies (braces) */
    TOK_LP = 27, TOK_RP = 28,           /* left and right parentheses */
    TOK_NAME = 29,                      /* identifier */
    TOK_NUMBER = 30,                    /* numeric constant */
    TOK_STRING = 31,                    /* string constant */
    TOK_OBJECT = 32,                    /* RegExp or other object constant */
    TOK_PRIMARY = 33,                   /* true, false, null, this, super */
    TOK_FUNCTION = 34,                  /* function keyword */
    TOK_EXPORT = 35,                    /* export keyword */
    TOK_IMPORT = 36,                    /* import keyword */
    TOK_IF = 37,                        /* if keyword */
    TOK_ELSE = 38,                      /* else keyword */
    TOK_SWITCH = 39,                    /* switch keyword */
    TOK_CASE = 40,                      /* case keyword */
    TOK_DEFAULT = 41,                   /* default keyword */
    TOK_WHILE = 42,                     /* while keyword */
    TOK_DO = 43,                        /* do keyword */
    TOK_FOR = 44,                       /* for keyword */
    TOK_BREAK = 45,                     /* break keyword */
    TOK_CONTINUE = 46,                  /* continue keyword */
    TOK_IN = 47,                        /* in keyword */
    TOK_VAR = 48,                       /* var keyword */
    TOK_WITH = 49,                      /* with keyword */
    TOK_RETURN = 50,                    /* return keyword */
    TOK_NEW = 51,                       /* new keyword */
    TOK_DELETE = 52,                    /* delete keyword */
    TOK_DEFSHARP = 53,                  /* #n= for object/array initializers */
    TOK_USESHARP = 54,                  /* #n# for object/array initializers */
    TOK_TRY = 55,                       /* try keyword */
    TOK_CATCH = 56,                     /* catch keyword */
    TOK_FINALLY = 57,                   /* finally keyword */
    TOK_THROW = 58,                     /* throw keyword */
    TOK_INSTANCEOF = 59,                /* instanceof keyword */
    TOK_DEBUGGER = 60,                  /* debugger keyword */
    TOK_XMLSTAGO = 61,                  /* XML start tag open (<) */
    TOK_XMLETAGO = 62,                  /* XML end tag open (</) */
    TOK_XMLPTAGC = 63,                  /* XML point tag close (/>) */
    TOK_XMLTAGC = 64,                   /* XML start or end tag close (>) */
    TOK_XMLNAME = 65,                   /* XML start-tag non-final fragment */
    TOK_XMLATTR = 66,                   /* XML quoted attribute value */
    TOK_XMLSPACE = 67,                  /* XML whitespace */
    TOK_XMLTEXT = 68,                   /* XML text */
    TOK_XMLCOMMENT = 69,                /* XML comment */
    TOK_XMLCDATA = 70,                  /* XML CDATA section */
    TOK_XMLPI = 71,                     /* XML processing instruction */
    TOK_AT = 72,                        /* XML attribute op (@) */
    TOK_DBLCOLON = 73,                  /* namespace qualified name op (::) */
    TOK_ANYNAME = 74,                   /* XML AnyName singleton (*) */
    TOK_DBLDOT = 75,                    /* XML descendant op (..) */
    TOK_FILTER = 76,                    /* XML filtering predicate op (.()) */
    TOK_XMLELEM = 77,                   /* XML element node type (no token) */
    TOK_XMLLIST = 78,                   /* XML list node type (no token) */
    TOK_YIELD = 79,                     /* yield from generator function */
    TOK_ARRAYCOMP = 80,                 /* array comprehension initialiser */
    TOK_ARRAYPUSH = 81,                 /* array push within comprehension */
    TOK_LEXICALSCOPE = 82,              /* block scope AST node label */
    TOK_LET = 83,                       /* let keyword */
    TOK_BODY = 84,                      /* synthetic body of function with
                                           destructuring formal parameters */
    TOK_RESERVED,                       /* reserved keywords */
    TOK_LIMIT                           /* domain size */
} JSTokenType;

#define IS_PRIMARY_TOKEN(tt) \
    ((uintN)((tt) - TOK_NAME) <= (uintN)(TOK_PRIMARY - TOK_NAME))

#define TOKEN_TYPE_IS_XML(tt) \
    (tt == TOK_AT || tt == TOK_DBLCOLON || tt == TOK_ANYNAME)

#if JS_HAS_BLOCK_SCOPE
# define TOKEN_TYPE_IS_DECL(tt) ((tt) == TOK_VAR || (tt) == TOK_LET)
#else
# define TOKEN_TYPE_IS_DECL(tt) ((tt) == TOK_VAR)
#endif

struct JSStringBuffer {
    jschar      *base;
    jschar      *limit;         /* length limit for quick bounds check */
    jschar      *ptr;           /* slot for next non-NUL char to store */
    void        *data;
    JSBool      (*grow)(JSStringBuffer *sb, size_t newlength);
    void        (*free)(JSStringBuffer *sb);
};

#define STRING_BUFFER_ERROR_BASE        ((jschar *) 1)
#define STRING_BUFFER_OK(sb)            ((sb)->base != STRING_BUFFER_ERROR_BASE)
#define STRING_BUFFER_OFFSET(sb)        ((sb)->ptr -(sb)->base)

extern void
js_InitStringBuffer(JSStringBuffer *sb);

extern void
js_FinishStringBuffer(JSStringBuffer *sb);

extern void
js_AppendChar(JSStringBuffer *sb, jschar c);

extern void
js_RepeatChar(JSStringBuffer *sb, jschar c, uintN count);

extern void
js_AppendCString(JSStringBuffer *sb, const char *asciiz);

extern void
js_AppendJSString(JSStringBuffer *sb, JSString *str);

struct JSTokenPtr {
    uint16              index;          /* index of char in physical line */
    uint16              lineno;         /* physical line number */
};

struct JSTokenPos {
    JSTokenPtr          begin;          /* first character and line of token */
    JSTokenPtr          end;            /* index 1 past last char, last line */
};

struct JSToken {
    JSTokenType         type;           /* char value or above enumerator */
    JSTokenPos          pos;            /* token position in file */
    jschar              *ptr;           /* beginning of token in line buffer */
    union {
        struct {                        /* non-numeric literal */
            JSOp        op;             /* operator, for minimal parser */
            JSAtom      *atom;          /* atom table entry */
        } s;
        struct {                        /* atom pair, for XML PIs */
            JSAtom      *atom2;         /* auxiliary atom table entry */
            JSAtom      *atom;          /* main atom table entry */
        } p;
        jsdouble        dval;           /* floating point number */
    } u;
};

#define t_op            u.s.op
#define t_atom          u.s.atom
#define t_atom2         u.p.atom2
#define t_dval          u.dval

typedef struct JSTokenBuf {
    jschar              *base;          /* base of line or stream buffer */
    jschar              *limit;         /* limit for quick bounds check */
    jschar              *ptr;           /* next char to get, or slot to use */
} JSTokenBuf;

#define JS_LINE_LIMIT   256             /* logical line buffer size limit --
                                           physical line length is unlimited */
#define NTOKENS         4               /* 1 current + 2 lookahead, rounded */
#define NTOKENS_MASK    (NTOKENS-1)     /* to power of 2 to avoid divmod by 3 */

struct JSTokenStream {
    JSToken             tokens[NTOKENS];/* circular token buffer */
    uintN               cursor;         /* index of last parsed token */
    uintN               lookahead;      /* count of lookahead tokens */
    uintN               lineno;         /* current line number */
    uintN               ungetpos;       /* next free char slot in ungetbuf */
    jschar              ungetbuf[6];    /* at most 6, for \uXXXX lookahead */
    uintN               flags;          /* flags -- see below */
    ptrdiff_t           linelen;        /* physical linebuf segment length */
    ptrdiff_t           linepos;        /* linebuf offset in physical line */
    JSTokenBuf          linebuf;        /* line buffer for diagnostics */
    JSTokenBuf          userbuf;        /* user input buffer if !file */
    JSStringBuffer      tokenbuf;       /* current token string buffer */
    const char          *filename;      /* input filename or null */
    FILE                *file;          /* stdio stream if reading from file */
    JSPrincipals        *principals;    /* principals associated with source */
    JSSourceHandler     listener;       /* callback for source; eg debugger */
    void                *listenerData;  /* listener 'this' data */
    void                *listenerTSData;/* listener data for this TokenStream */
    jschar              *saveEOL;       /* save next end of line in userbuf, to
                                           optimize for very long lines */
};

#define CURRENT_TOKEN(ts)       ((ts)->tokens[(ts)->cursor])
#define ON_CURRENT_LINE(ts,pos) ((uint16)(ts)->lineno == (pos).end.lineno)

/* JSTokenStream flags */
#define TSF_ERROR       0x01            /* fatal error while compiling */
#define TSF_EOF         0x02            /* hit end of file */
#define TSF_NEWLINES    0x04            /* tokenize newlines */
#define TSF_OPERAND     0x08            /* looking for operand, not operator */
#define TSF_NLFLAG      0x20            /* last linebuf ended with \n */
#define TSF_CRFLAG      0x40            /* linebuf would have ended with \r */
#define TSF_DIRTYLINE   0x80            /* non-whitespace since start of line */
#define TSF_OWNFILENAME 0x100           /* ts->filename is malloc'd */
#define TSF_XMLTAGMODE  0x200           /* scanning within an XML tag in E4X */
#define TSF_XMLTEXTMODE 0x400           /* scanning XMLText terminal from E4X */
#define TSF_XMLONLYMODE 0x800           /* don't scan {expr} within text/tag */

/* Flag indicating unexpected end of input, i.e. TOK_EOF not at top-level. */
#define TSF_UNEXPECTED_EOF 0x1000

/*
 * To handle the hard case of contiguous HTML comments, we want to clear the
 * TSF_DIRTYINPUT flag at the end of each such comment.  But we'd rather not
 * scan for --> within every //-style comment unless we have to.  So we set
 * TSF_IN_HTML_COMMENT when a <!-- is scanned as an HTML begin-comment, and
 * clear it (and TSF_DIRTYINPUT) when we scan --> either on a clean line, or
 * only if (ts->flags & TSF_IN_HTML_COMMENT), in a //-style comment.
 *
 * This still works as before given a malformed comment hiding hack such as:
 *
 *    <script>
 *      <!-- comment hiding hack #1
 *      code goes here
 *      // --> oops, markup for script-unaware browsers goes here!
 *    </script>
 *
 * It does not cope with malformed comment hiding hacks where --> is hidden
 * by C-style comments, or on a dirty line.  Such cases are already broken.
 */
#define TSF_IN_HTML_COMMENT 0x2000

/* Ignore keywords and return TOK_NAME instead to the parser. */
#define TSF_KEYWORD_IS_NAME 0x4000

/* Unicode separators that are treated as line terminators, in addition to \n, \r */
#define LINE_SEPARATOR  0x2028
#define PARA_SEPARATOR  0x2029

/*
 * Create a new token stream, either from an input buffer or from a file.
 * Return null on file-open or memory-allocation failure.
 *
 * NB: All of js_New{,Buffer,File}TokenStream() return a pointer to transient
 * memory in the current context's temp pool.  This memory is deallocated via
 * JS_ARENA_RELEASE() after parsing is finished.
 */
extern JSTokenStream *
js_NewTokenStream(JSContext *cx, const jschar *base, size_t length,
                  const char *filename, uintN lineno, JSPrincipals *principals);

extern JS_FRIEND_API(JSTokenStream *)
js_NewBufferTokenStream(JSContext *cx, const jschar *base, size_t length);

extern JS_FRIEND_API(JSTokenStream *)
js_NewFileTokenStream(JSContext *cx, const char *filename, FILE *defaultfp);

extern JS_FRIEND_API(JSBool)
js_CloseTokenStream(JSContext *cx, JSTokenStream *ts);

extern JS_FRIEND_API(int)
js_fgets(char *buf, int size, FILE *file);

/*
 * If the given char array forms JavaScript keyword, return corresponding
 * token. Otherwise return TOK_EOF.
 */
extern JSTokenType
js_CheckKeyword(const jschar *chars, size_t length);

#define js_IsKeyword(chars, length) \
    (js_CheckKeyword(chars, length) != TOK_EOF)

/*
 * Friend-exported API entry point to call a mapping function on each reserved
 * identifier in the scanner's keyword table.
 */
extern JS_FRIEND_API(void)
js_MapKeywords(void (*mapfun)(const char *));

/*
 * Report a compile-time error by its number, using ts or cg to show context.
 * Return true for a warning, false for an error.
 */
extern JSBool
js_ReportCompileErrorNumber(JSContext *cx, void *handle, uintN flags,
                            uintN errorNumber, ...);

extern JSBool
js_ReportCompileErrorNumberUC(JSContext *cx, void *handle, uintN flags,
                              uintN errorNumber, ...);

/* Steal some JSREPORT_* bits (see jsapi.h) to tell handle's type. */
#define JSREPORT_HANDLE 0x300
#define JSREPORT_TS     0x000
#define JSREPORT_CG     0x100
#define JSREPORT_PN     0x200

/*
 * Look ahead one token and return its type.
 */
extern JSTokenType
js_PeekToken(JSContext *cx, JSTokenStream *ts);

extern JSTokenType
js_PeekTokenSameLine(JSContext *cx, JSTokenStream *ts);

/*
 * Get the next token from ts.
 */
extern JSTokenType
js_GetToken(JSContext *cx, JSTokenStream *ts);

/*
 * Push back the last scanned token onto ts.
 */
extern void
js_UngetToken(JSTokenStream *ts);

/*
 * Get the next token from ts if its type is tt.
 */
extern JSBool
js_MatchToken(JSContext *cx, JSTokenStream *ts, JSTokenType tt);

JS_END_EXTERN_C

#endif /* jsscan_h___ */
