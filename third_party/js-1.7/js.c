/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sw=4 et tw=78:
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

/*
 * JS shell.
 */
#include "jsstddef.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <locale.h>
#include "jstypes.h"
#include "jsarena.h"
#include "jsutil.h"
#include "jsprf.h"
#include "jsapi.h"
#include "jsatom.h"
#include "jscntxt.h"
#include "jsdbgapi.h"
#include "jsemit.h"
#include "jsfun.h"
#include "jsgc.h"
#include "jslock.h"
#include "jsobj.h"
#include "jsparse.h"
#include "jsscope.h"
#include "jsscript.h"

#ifdef PERLCONNECT
#include "perlconnect/jsperl.h"
#endif

#ifdef LIVECONNECT
#include "jsjava.h"
#endif

#ifdef JSDEBUGGER
#include "jsdebug.h"
#ifdef JSDEBUGGER_JAVA_UI
#include "jsdjava.h"
#endif /* JSDEBUGGER_JAVA_UI */
#ifdef JSDEBUGGER_C_UI
#include "jsdb.h"
#endif /* JSDEBUGGER_C_UI */
#endif /* JSDEBUGGER */

#ifdef XP_UNIX
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#endif

#if defined(XP_WIN) || defined(XP_OS2)
#include <io.h>     /* for isatty() */
#endif

typedef enum JSShellExitCode {
    EXITCODE_RUNTIME_ERROR      = 3,
    EXITCODE_FILE_NOT_FOUND     = 4,
    EXITCODE_OUT_OF_MEMORY      = 5
} JSShellExitCode;

size_t gStackChunkSize = 8192;

/* Assume that we can not use more than 5e5 bytes of C stack by default. */
static size_t gMaxStackSize = 500000;

static jsuword gStackBase;
int gExitCode = 0;
JSBool gQuitting = JS_FALSE;
FILE *gErrFile = NULL;
FILE *gOutFile = NULL;

#ifdef JSDEBUGGER
static JSDContext *_jsdc;
#ifdef JSDEBUGGER_JAVA_UI
static JSDJContext *_jsdjc;
#endif /* JSDEBUGGER_JAVA_UI */
#endif /* JSDEBUGGER */

static JSBool reportWarnings = JS_TRUE;
static JSBool compileOnly = JS_FALSE;

typedef enum JSShellErrNum {
#define MSG_DEF(name, number, count, exception, format) \
    name = number,
#include "jsshell.msg"
#undef MSG_DEF
    JSShellErr_Limit
#undef MSGDEF
} JSShellErrNum;

static const JSErrorFormatString *
my_GetErrorMessage(void *userRef, const char *locale, const uintN errorNumber);
static JSObject *
split_setup(JSContext *cx);

#ifdef EDITLINE
extern char     *readline(const char *prompt);
extern void     add_history(char *line);
#endif

static JSBool
GetLine(JSContext *cx, char *bufp, FILE *file, const char *prompt) {
#ifdef EDITLINE
    /*
     * Use readline only if file is stdin, because there's no way to specify
     * another handle.  Are other filehandles interactive?
     */
    if (file == stdin) {
        char *linep = readline(prompt);
        if (!linep)
            return JS_FALSE;
        if (linep[0] != '\0')
            add_history(linep);
        strcpy(bufp, linep);
        JS_free(cx, linep);
        bufp += strlen(bufp);
        *bufp++ = '\n';
        *bufp = '\0';
    } else
#endif
    {
        char line[256];
        fprintf(gOutFile, prompt);
        fflush(gOutFile);
        if (!fgets(line, sizeof line, file))
            return JS_FALSE;
        strcpy(bufp, line);
    }
    return JS_TRUE;
}

static void
Process(JSContext *cx, JSObject *obj, char *filename, JSBool forceTTY)
{
    JSBool ok, hitEOF;
    JSScript *script;
    jsval result;
    JSString *str;
    char buffer[4096];
    char *bufp;
    int lineno;
    int startline;
    FILE *file;
    jsuword stackLimit;

    if (forceTTY || !filename || strcmp(filename, "-") == 0) {
        file = stdin;
    } else {
        file = fopen(filename, "r");
        if (!file) {
            JS_ReportErrorNumber(cx, my_GetErrorMessage, NULL,
                                 JSSMSG_CANT_OPEN, filename, strerror(errno));
            gExitCode = EXITCODE_FILE_NOT_FOUND;
            return;
        }
    }

    if (gMaxStackSize == 0) {
        /*
         * Disable checking for stack overflow if limit is zero.
         */
        stackLimit = 0;
    } else {
#if JS_STACK_GROWTH_DIRECTION > 0
        stackLimit = gStackBase + gMaxStackSize;
#else
        stackLimit = gStackBase - gMaxStackSize;
#endif
    }
    JS_SetThreadStackLimit(cx, stackLimit);

    if (!forceTTY && !isatty(fileno(file))) {
        /*
         * It's not interactive - just execute it.
         *
         * Support the UNIX #! shell hack; gobble the first line if it starts
         * with '#'.  TODO - this isn't quite compatible with sharp variables,
         * as a legal js program (using sharp variables) might start with '#'.
         * But that would require multi-character lookahead.
         */
        int ch = fgetc(file);
        if (ch == '#') {
            while((ch = fgetc(file)) != EOF) {
                if (ch == '\n' || ch == '\r')
                    break;
            }
        }
        ungetc(ch, file);
        script = JS_CompileFileHandle(cx, obj, filename, file);
        if (script) {
            if (!compileOnly)
                (void)JS_ExecuteScript(cx, obj, script, &result);
            JS_DestroyScript(cx, script);
        }

        return;
    }

    /* It's an interactive filehandle; drop into read-eval-print loop. */
    lineno = 1;
    hitEOF = JS_FALSE;
    do {
        bufp = buffer;
        *bufp = '\0';

        /*
         * Accumulate lines until we get a 'compilable unit' - one that either
         * generates an error (before running out of source) or that compiles
         * cleanly.  This should be whenever we get a complete statement that
         * coincides with the end of a line.
         */
        startline = lineno;
        do {
            if (!GetLine(cx, bufp, file, startline == lineno ? "js> " : "")) {
                hitEOF = JS_TRUE;
                break;
            }
            bufp += strlen(bufp);
            lineno++;
        } while (!JS_BufferIsCompilableUnit(cx, obj, buffer, strlen(buffer)));

        /* Clear any pending exception from previous failed compiles.  */
        JS_ClearPendingException(cx);
        script = JS_CompileScript(cx, obj, buffer, strlen(buffer), "typein",
                                  startline);
        if (script) {
            if (!compileOnly) {
                ok = JS_ExecuteScript(cx, obj, script, &result);
                if (ok && result != JSVAL_VOID) {
                    str = JS_ValueToString(cx, result);
                    if (str)
                        fprintf(gOutFile, "%s\n", JS_GetStringBytes(str));
                    else
                        ok = JS_FALSE;
                }
            }
            JS_DestroyScript(cx, script);
        }
    } while (!hitEOF && !gQuitting);
    fprintf(gOutFile, "\n");
    return;
}

static int
usage(void)
{
    fprintf(gErrFile, "%s\n", JS_GetImplementationVersion());
    fprintf(gErrFile, "usage: js [-PswWxCi] [-b branchlimit] [-c stackchunksize] [-v version] [-f scriptfile] [-e script] [-S maxstacksize] [scriptfile] [scriptarg...]\n");
    return 2;
}

static uint32 gBranchCount;
static uint32 gBranchLimit;

static JSBool
my_BranchCallback(JSContext *cx, JSScript *script)
{
    if (++gBranchCount == gBranchLimit) {
        if (script) {
            if (script->filename)
                fprintf(gErrFile, "%s:", script->filename);
            fprintf(gErrFile, "%u: script branch callback (%u callbacks)\n",
                    script->lineno, gBranchLimit);
        } else {
            fprintf(gErrFile, "native branch callback (%u callbacks)\n",
                    gBranchLimit);
        }
        gBranchCount = 0;
        return JS_FALSE;
    }
    if ((gBranchCount & 0x3fff) == 1)
        JS_MaybeGC(cx);
    return JS_TRUE;
}

extern JSClass global_class;

static int
ProcessArgs(JSContext *cx, JSObject *obj, char **argv, int argc)
{
    int i, j, length;
    JSObject *argsObj;
    char *filename = NULL;
    JSBool isInteractive = JS_TRUE;
    JSBool forceTTY = JS_FALSE;

    /*
     * Scan past all optional arguments so we can create the arguments object
     * before processing any -f options, which must interleave properly with
     * -v and -w options.  This requires two passes, and without getopt, we'll
     * have to keep the option logic here and in the second for loop in sync.
     */
    for (i = 0; i < argc; i++) {
        if (argv[i][0] != '-' || argv[i][1] == '\0') {
            ++i;
            break;
        }
        switch (argv[i][1]) {
          case 'b':
          case 'c':
          case 'f':
          case 'e':
          case 'v':
          case 'S':
            ++i;
            break;
          default:;
        }
    }

    /*
     * Create arguments early and define it to root it, so it's safe from any
     * GC calls nested below, and so it is available to -f <file> arguments.
     */
    argsObj = JS_NewArrayObject(cx, 0, NULL);
    if (!argsObj)
        return 1;
    if (!JS_DefineProperty(cx, obj, "arguments", OBJECT_TO_JSVAL(argsObj),
                           NULL, NULL, 0)) {
        return 1;
    }

    length = argc - i;
    for (j = 0; j < length; j++) {
        JSString *str = JS_NewStringCopyZ(cx, argv[i++]);
        if (!str)
            return 1;
        if (!JS_DefineElement(cx, argsObj, j, STRING_TO_JSVAL(str),
                              NULL, NULL, JSPROP_ENUMERATE)) {
            return 1;
        }
    }

    for (i = 0; i < argc; i++) {
        if (argv[i][0] != '-' || argv[i][1] == '\0') {
            filename = argv[i++];
            isInteractive = JS_FALSE;
            break;
        }

        switch (argv[i][1]) {
        case 'v':
            if (++i == argc)
                return usage();

            JS_SetVersion(cx, (JSVersion) atoi(argv[i]));
            break;

        case 'w':
            reportWarnings = JS_TRUE;
            break;

        case 'W':
            reportWarnings = JS_FALSE;
            break;

        case 's':
            JS_ToggleOptions(cx, JSOPTION_STRICT);
            break;

        case 'x':
            JS_ToggleOptions(cx, JSOPTION_XML);
            break;

        case 'P':
            if (JS_GET_CLASS(cx, JS_GetPrototype(cx, obj)) != &global_class) {
                JSObject *gobj;

                if (!JS_SealObject(cx, obj, JS_TRUE))
                    return JS_FALSE;
                gobj = JS_NewObject(cx, &global_class, NULL, NULL);
                if (!gobj)
                    return JS_FALSE;
                if (!JS_SetPrototype(cx, gobj, obj))
                    return JS_FALSE;
                JS_SetParent(cx, gobj, NULL);
                JS_SetGlobalObject(cx, gobj);
                obj = gobj;
            }
            break;

        case 'b':
            gBranchLimit = atoi(argv[++i]);
            JS_SetBranchCallback(cx, my_BranchCallback);
            JS_ToggleOptions(cx, JSOPTION_NATIVE_BRANCH_CALLBACK);
            break;

        case 'c':
            /* set stack chunk size */
            gStackChunkSize = atoi(argv[++i]);
            break;

        case 'f':
            if (++i == argc)
                return usage();

            Process(cx, obj, argv[i], JS_FALSE);

            /*
             * XXX: js -f foo.js should interpret foo.js and then
             * drop into interactive mode, but that breaks the test
             * harness. Just execute foo.js for now.
             */
            isInteractive = JS_FALSE;
            break;

        case 'e':
        {
            jsval rval;

            if (++i == argc)
                return usage();

            /* Pass a filename of -e to imitate PERL */
            JS_EvaluateScript(cx, obj, argv[i], strlen(argv[i]),
                              "-e", 1, &rval);

            isInteractive = JS_FALSE;
            break;

        }
        case 'C':
            compileOnly = JS_TRUE;
            isInteractive = JS_FALSE;
            break;

        case 'i':
            isInteractive = forceTTY = JS_TRUE;
            break;

        case 'S':
            if (++i == argc)
                return usage();

            /* Set maximum stack size. */
            gMaxStackSize = atoi(argv[i]);
            break;

        case 'z':
            obj = split_setup(cx);
            break;

        default:
            return usage();
        }
    }

    if (filename || isInteractive)
        Process(cx, obj, filename, forceTTY);
    return gExitCode;
}


static JSBool
Version(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval)
{
    if (argc > 0 && JSVAL_IS_INT(argv[0]))
        *rval = INT_TO_JSVAL(JS_SetVersion(cx, (JSVersion) JSVAL_TO_INT(argv[0])));
    else
        *rval = INT_TO_JSVAL(JS_GetVersion(cx));
    return JS_TRUE;
}

static struct {
    const char  *name;
    uint32      flag;
} js_options[] = {
    {"strict",          JSOPTION_STRICT},
    {"werror",          JSOPTION_WERROR},
    {"atline",          JSOPTION_ATLINE},
    {"xml",             JSOPTION_XML},
    {0,                 0}
};

static JSBool
Options(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval)
{
    uint32 optset, flag;
    uintN i, j, found;
    JSString *str;
    const char *opt;
    char *names;

    optset = 0;
    for (i = 0; i < argc; i++) {
        str = JS_ValueToString(cx, argv[i]);
        if (!str)
            return JS_FALSE;
        opt = JS_GetStringBytes(str);
        for (j = 0; js_options[j].name; j++) {
            if (strcmp(js_options[j].name, opt) == 0) {
                optset |= js_options[j].flag;
                break;
            }
        }
    }
    optset = JS_ToggleOptions(cx, optset);

    names = NULL;
    found = 0;
    while (optset != 0) {
        flag = optset;
        optset &= optset - 1;
        flag &= ~optset;
        for (j = 0; js_options[j].name; j++) {
            if (js_options[j].flag == flag) {
                names = JS_sprintf_append(names, "%s%s",
                                          names ? "," : "", js_options[j].name);
                found++;
                break;
            }
        }
    }
    if (!found)
        names = strdup("");
    if (!names) {
        JS_ReportOutOfMemory(cx);
        return JS_FALSE;
    }

    str = JS_NewString(cx, names, strlen(names));
    if (!str) {
        free(names);
        return JS_FALSE;
    }
    *rval = STRING_TO_JSVAL(str);
    return JS_TRUE;
}

static JSBool
Load(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval)
{
    uintN i;
    JSString *str;
    const char *filename;
    JSScript *script;
    JSBool ok;
    jsval result;
    uint32 oldopts;

    for (i = 0; i < argc; i++) {
        str = JS_ValueToString(cx, argv[i]);
        if (!str)
            return JS_FALSE;
        argv[i] = STRING_TO_JSVAL(str);
        filename = JS_GetStringBytes(str);
        errno = 0;
        oldopts = JS_GetOptions(cx);
        JS_SetOptions(cx, oldopts | JSOPTION_COMPILE_N_GO);
        script = JS_CompileFile(cx, obj, filename);
        if (!script) {
            ok = JS_FALSE;
        } else {
            ok = !compileOnly
                 ? JS_ExecuteScript(cx, obj, script, &result)
                 : JS_TRUE;
            JS_DestroyScript(cx, script);
        }
        JS_SetOptions(cx, oldopts);
        if (!ok)
            return JS_FALSE;
    }

    return JS_TRUE;
}

/*
 * function readline()
 * Provides a hook for scripts to read a line from stdin.
 */
static JSBool
ReadLine(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval)
{
#define BUFSIZE 256
    FILE *from;
    char *buf, *tmp;
    size_t bufsize, buflength, gotlength;
    JSString *str;

    from = stdin;
    buflength = 0;
    bufsize = BUFSIZE;
    buf = JS_malloc(cx, bufsize);
    if (!buf)
        return JS_FALSE;

    while ((gotlength =
            js_fgets(buf + buflength, bufsize - buflength, from)) > 0) {
        buflength += gotlength;

        /* Are we done? */
        if (buf[buflength - 1] == '\n') {
            buf[buflength - 1] = '\0';
            break;
        }

        /* Else, grow our buffer for another pass. */
        tmp = JS_realloc(cx, buf, bufsize * 2);
        if (!tmp) {
            JS_free(cx, buf);
            return JS_FALSE;
        }

        bufsize *= 2;
        buf = tmp;
    }

    /* Treat the empty string specially. */
    if (buflength == 0) {
        *rval = JS_GetEmptyStringValue(cx);
        JS_free(cx, buf);
        return JS_TRUE;
    }

    /* Shrink the buffer to the real size. */
    tmp = JS_realloc(cx, buf, buflength);
    if (!tmp) {
        JS_free(cx, buf);
        return JS_FALSE;
    }

    buf = tmp;

    /*
     * Turn buf into a JSString. Note that buflength includes the trailing null
     * character.
     */
    str = JS_NewString(cx, buf, buflength - 1);
    if (!str) {
        JS_free(cx, buf);
        return JS_FALSE;
    }

    *rval = STRING_TO_JSVAL(str);
    return JS_TRUE;
}

static JSBool
Print(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval)
{
    uintN i, n;
    JSString *str;

    for (i = n = 0; i < argc; i++) {
        str = JS_ValueToString(cx, argv[i]);
        if (!str)
            return JS_FALSE;
        fprintf(gOutFile, "%s%s", i ? " " : "", JS_GetStringBytes(str));
    }
    n++;
    if (n)
        fputc('\n', gOutFile);
    return JS_TRUE;
}

static JSBool
Help(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval);

static JSBool
Quit(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval)
{
#ifdef LIVECONNECT
    JSJ_SimpleShutdown();
#endif

    JS_ConvertArguments(cx, argc, argv,"/ i", &gExitCode);

    gQuitting = JS_TRUE;
    return JS_FALSE;
}

static JSBool
GC(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval)
{
    JSRuntime *rt;
    uint32 preBytes;

    rt = cx->runtime;
    preBytes = rt->gcBytes;
#ifdef GC_MARK_DEBUG
    if (argc && JSVAL_IS_STRING(argv[0])) {
        char *name = JS_GetStringBytes(JSVAL_TO_STRING(argv[0]));
        FILE *file = fopen(name, "w");
        if (!file) {
            fprintf(gErrFile, "gc: can't open %s: %s\n", strerror(errno));
            return JS_FALSE;
        }
        js_DumpGCHeap = file;
    } else {
        js_DumpGCHeap = stdout;
    }
#endif
    JS_GC(cx);
#ifdef GC_MARK_DEBUG
    if (js_DumpGCHeap != stdout)
        fclose(js_DumpGCHeap);
    js_DumpGCHeap = NULL;
#endif
    fprintf(gOutFile, "before %lu, after %lu, break %08lx\n",
            (unsigned long)preBytes, (unsigned long)rt->gcBytes,
#ifdef XP_UNIX
            (unsigned long)sbrk(0)
#else
            0
#endif
            );
#ifdef JS_GCMETER
    js_DumpGCStats(rt, stdout);
#endif
    return JS_TRUE;
}

static JSScript *
ValueToScript(JSContext *cx, jsval v)
{
    JSScript *script;
    JSFunction *fun;

    if (!JSVAL_IS_PRIMITIVE(v) &&
        JS_GET_CLASS(cx, JSVAL_TO_OBJECT(v)) == &js_ScriptClass) {
        script = (JSScript *) JS_GetPrivate(cx, JSVAL_TO_OBJECT(v));
    } else {
        fun = JS_ValueToFunction(cx, v);
        if (!fun)
            return NULL;
        script = FUN_SCRIPT(fun);
    }
    return script;
}

static JSBool
GetTrapArgs(JSContext *cx, uintN argc, jsval *argv, JSScript **scriptp,
            int32 *ip)
{
    jsval v;
    uintN intarg;
    JSScript *script;

    *scriptp = cx->fp->down->script;
    *ip = 0;
    if (argc != 0) {
        v = argv[0];
        intarg = 0;
        if (!JSVAL_IS_PRIMITIVE(v) &&
            (JS_GET_CLASS(cx, JSVAL_TO_OBJECT(v)) == &js_FunctionClass ||
             JS_GET_CLASS(cx, JSVAL_TO_OBJECT(v)) == &js_ScriptClass)) {
            script = ValueToScript(cx, v);
            if (!script)
                return JS_FALSE;
            *scriptp = script;
            intarg++;
        }
        if (argc > intarg) {
            if (!JS_ValueToInt32(cx, argv[intarg], ip))
                return JS_FALSE;
        }
    }
    return JS_TRUE;
}

static JSTrapStatus
TrapHandler(JSContext *cx, JSScript *script, jsbytecode *pc, jsval *rval,
            void *closure)
{
    JSString *str;
    JSStackFrame *caller;

    str = (JSString *) closure;
    caller = JS_GetScriptedCaller(cx, NULL);
    if (!JS_EvaluateScript(cx, caller->scopeChain,
                           JS_GetStringBytes(str), JS_GetStringLength(str),
                           caller->script->filename, caller->script->lineno,
                           rval)) {
        return JSTRAP_ERROR;
    }
    if (*rval != JSVAL_VOID)
        return JSTRAP_RETURN;
    return JSTRAP_CONTINUE;
}

static JSBool
Trap(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval)
{
    JSString *str;
    JSScript *script;
    int32 i;

    if (argc == 0) {
        JS_ReportErrorNumber(cx, my_GetErrorMessage, NULL, JSSMSG_TRAP_USAGE);
        return JS_FALSE;
    }
    argc--;
    str = JS_ValueToString(cx, argv[argc]);
    if (!str)
        return JS_FALSE;
    argv[argc] = STRING_TO_JSVAL(str);
    if (!GetTrapArgs(cx, argc, argv, &script, &i))
        return JS_FALSE;
    return JS_SetTrap(cx, script, script->code + i, TrapHandler, str);
}

static JSBool
Untrap(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval)
{
    JSScript *script;
    int32 i;

    if (!GetTrapArgs(cx, argc, argv, &script, &i))
        return JS_FALSE;
    JS_ClearTrap(cx, script, script->code + i, NULL, NULL);
    return JS_TRUE;
}

static JSBool
LineToPC(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval)
{
    JSScript *script;
    int32 i;
    uintN lineno;
    jsbytecode *pc;

    if (argc == 0) {
        JS_ReportErrorNumber(cx, my_GetErrorMessage, NULL, JSSMSG_LINE2PC_USAGE);
        return JS_FALSE;
    }
    script = cx->fp->down->script;
    if (!GetTrapArgs(cx, argc, argv, &script, &i))
        return JS_FALSE;
    lineno = (i == 0) ? script->lineno : (uintN)i;
    pc = JS_LineNumberToPC(cx, script, lineno);
    if (!pc)
        return JS_FALSE;
    *rval = INT_TO_JSVAL(PTRDIFF(pc, script->code, jsbytecode));
    return JS_TRUE;
}

static JSBool
PCToLine(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval)
{
    JSScript *script;
    int32 i;
    uintN lineno;

    if (!GetTrapArgs(cx, argc, argv, &script, &i))
        return JS_FALSE;
    lineno = JS_PCToLineNumber(cx, script, script->code + i);
    if (!lineno)
        return JS_FALSE;
    *rval = INT_TO_JSVAL(lineno);
    return JS_TRUE;
}

#ifdef DEBUG

static void
GetSwitchTableBounds(JSScript *script, uintN offset,
                     uintN *start, uintN *end)
{
    jsbytecode *pc;
    JSOp op;
    ptrdiff_t jmplen;
    jsint low, high, n;

    pc = script->code + offset;
    op = *pc;
    switch (op) {
      case JSOP_TABLESWITCHX:
        jmplen = JUMPX_OFFSET_LEN;
        goto jump_table;
      case JSOP_TABLESWITCH:
        jmplen = JUMP_OFFSET_LEN;
      jump_table:
        pc += jmplen;
        low = GET_JUMP_OFFSET(pc);
        pc += JUMP_OFFSET_LEN;
        high = GET_JUMP_OFFSET(pc);
        pc += JUMP_OFFSET_LEN;
        n = high - low + 1;
        break;

      case JSOP_LOOKUPSWITCHX:
        jmplen = JUMPX_OFFSET_LEN;
        goto lookup_table;
      default:
        JS_ASSERT(op == JSOP_LOOKUPSWITCH);
        jmplen = JUMP_OFFSET_LEN;
      lookup_table:
        pc += jmplen;
        n = GET_ATOM_INDEX(pc);
        pc += ATOM_INDEX_LEN;
        jmplen += ATOM_INDEX_LEN;
        break;
    }

    *start = (uintN)(pc - script->code);
    *end = *start + (uintN)(n * jmplen);
}


/*
 * SrcNotes assumes that SRC_METHODBASE should be distinguished from SRC_LABEL
 * using the bytecode the source note points to.
 */
JS_STATIC_ASSERT(SRC_LABEL == SRC_METHODBASE);

static void
SrcNotes(JSContext *cx, JSScript *script)
{
    uintN offset, delta, caseOff, switchTableStart, switchTableEnd;
    jssrcnote *notes, *sn;
    JSSrcNoteType type;
    const char *name;
    JSOp op;
    jsatomid atomIndex;
    JSAtom *atom;

    fprintf(gOutFile, "\nSource notes:\n");
    offset = 0;
    notes = SCRIPT_NOTES(script);
    switchTableEnd = switchTableStart = 0;
    for (sn = notes; !SN_IS_TERMINATOR(sn); sn = SN_NEXT(sn)) {
        delta = SN_DELTA(sn);
        offset += delta;
        type = (JSSrcNoteType) SN_TYPE(sn);
        name = js_SrcNoteSpec[type].name;
        if (type == SRC_LABEL) {
            /* Heavily overloaded case. */
            if (switchTableStart <= offset && offset < switchTableEnd) {
                name = "case";
            } else {
                op = script->code[offset];
                if (op == JSOP_GETMETHOD || op == JSOP_SETMETHOD) {
                    /* This is SRC_METHODBASE which we print as SRC_PCBASE. */
                    type = SRC_PCBASE;
                    name = "methodbase";
                } else {
                    JS_ASSERT(op == JSOP_NOP);
                }
            }
        }
        fprintf(gOutFile, "%3u: %5u [%4u] %-8s",
                PTRDIFF(sn, notes, jssrcnote), offset, delta, name);
        switch (type) {
          case SRC_SETLINE:
            fprintf(gOutFile, " lineno %u", (uintN) js_GetSrcNoteOffset(sn, 0));
            break;
          case SRC_FOR:
            fprintf(gOutFile, " cond %u update %u tail %u",
                   (uintN) js_GetSrcNoteOffset(sn, 0),
                   (uintN) js_GetSrcNoteOffset(sn, 1),
                   (uintN) js_GetSrcNoteOffset(sn, 2));
            break;
          case SRC_IF_ELSE:
            fprintf(gOutFile, " else %u elseif %u",
                   (uintN) js_GetSrcNoteOffset(sn, 0),
                   (uintN) js_GetSrcNoteOffset(sn, 1));
            break;
          case SRC_COND:
          case SRC_WHILE:
          case SRC_PCBASE:
          case SRC_PCDELTA:
          case SRC_DECL:
          case SRC_BRACE:
            fprintf(gOutFile, " offset %u", (uintN) js_GetSrcNoteOffset(sn, 0));
            break;
          case SRC_LABEL:
          case SRC_LABELBRACE:
          case SRC_BREAK2LABEL:
          case SRC_CONT2LABEL:
          case SRC_FUNCDEF: {
            const char *bytes;
            JSFunction *fun;
            JSString *str;

            atomIndex = (jsatomid) js_GetSrcNoteOffset(sn, 0);
            atom = js_GetAtom(cx, &script->atomMap, atomIndex);
            if (type != SRC_FUNCDEF) {
                bytes = js_AtomToPrintableString(cx, atom);
            } else {
                fun = (JSFunction *)
                    JS_GetPrivate(cx, ATOM_TO_OBJECT(atom));
                str = JS_DecompileFunction(cx, fun, JS_DONT_PRETTY_PRINT);
                bytes = str ? JS_GetStringBytes(str) : "N/A";
            }
            fprintf(gOutFile, " atom %u (%s)", (uintN)atomIndex, bytes);
            break;
          }
          case SRC_SWITCH:
            fprintf(gOutFile, " length %u", (uintN) js_GetSrcNoteOffset(sn, 0));
            caseOff = (uintN) js_GetSrcNoteOffset(sn, 1);
            if (caseOff)
                fprintf(gOutFile, " first case offset %u", caseOff);
            GetSwitchTableBounds(script, offset,
                                 &switchTableStart, &switchTableEnd);
            break;
          case SRC_CATCH:
            delta = (uintN) js_GetSrcNoteOffset(sn, 0);
            if (delta) {
                if (script->main[offset] == JSOP_LEAVEBLOCK)
                    fprintf(gOutFile, " stack depth %u", delta);
                else
                    fprintf(gOutFile, " guard delta %u", delta);
            }
            break;
          default:;
        }
        fputc('\n', gOutFile);
    }
}

static JSBool
Notes(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval)
{
    uintN i;
    JSScript *script;

    for (i = 0; i < argc; i++) {
        script = ValueToScript(cx, argv[i]);
        if (!script)
            continue;

        SrcNotes(cx, script);
    }
    return JS_TRUE;
}

static JSBool
TryNotes(JSContext *cx, JSScript *script)
{
    JSTryNote *tn = script->trynotes;

    if (!tn)
        return JS_TRUE;
    fprintf(gOutFile, "\nException table:\nstart\tend\tcatch\n");
    while (tn->start && tn->catchStart) {
        fprintf(gOutFile, "  %d\t%d\t%d\n",
               tn->start, tn->start + tn->length, tn->catchStart);
        tn++;
    }
    return JS_TRUE;
}

static JSBool
Disassemble(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval)
{
    JSBool lines;
    uintN i;
    JSScript *script;

    if (argc > 0 &&
        JSVAL_IS_STRING(argv[0]) &&
        !strcmp(JS_GetStringBytes(JSVAL_TO_STRING(argv[0])), "-l")) {
        lines = JS_TRUE;
        argv++, argc--;
    } else {
        lines = JS_FALSE;
    }
    for (i = 0; i < argc; i++) {
        script = ValueToScript(cx, argv[i]);
        if (!script)
            return JS_FALSE;

        if (VALUE_IS_FUNCTION(cx, argv[i])) {
            JSFunction *fun = JS_ValueToFunction(cx, argv[i]);
            if (fun && (fun->flags & JSFUN_FLAGS_MASK)) {
                uint16 flags = fun->flags;
                fputs("flags:", stdout);

#define SHOW_FLAG(flag) if (flags & JSFUN_##flag) fputs(" " #flag, stdout);

                SHOW_FLAG(LAMBDA);
                SHOW_FLAG(SETTER);
                SHOW_FLAG(GETTER);
                SHOW_FLAG(BOUND_METHOD);
                SHOW_FLAG(HEAVYWEIGHT);
                SHOW_FLAG(THISP_STRING);
                SHOW_FLAG(THISP_NUMBER);
                SHOW_FLAG(THISP_BOOLEAN);
                SHOW_FLAG(INTERPRETED);

#undef SHOW_FLAG
                putchar('\n');
            }
        }

        if (!js_Disassemble(cx, script, lines, stdout))
            return JS_FALSE;
        SrcNotes(cx, script);
        TryNotes(cx, script);
    }
    return JS_TRUE;
}

static JSBool
DisassWithSrc(JSContext *cx, JSObject *obj, uintN argc, jsval *argv,
              jsval *rval)
{
#define LINE_BUF_LEN 512
    uintN i, len, line1, line2, bupline;
    JSScript *script;
    FILE *file;
    char linebuf[LINE_BUF_LEN];
    jsbytecode *pc, *end;
    static char sep[] = ";-------------------------";

    for (i = 0; i < argc; i++) {
        script = ValueToScript(cx, argv[i]);
        if (!script)
            return JS_FALSE;

        if (!script || !script->filename) {
            JS_ReportErrorNumber(cx, my_GetErrorMessage, NULL,
                                            JSSMSG_FILE_SCRIPTS_ONLY);
            return JS_FALSE;
        }

        file = fopen(script->filename, "r");
        if (!file) {
            JS_ReportErrorNumber(cx, my_GetErrorMessage, NULL,
                            JSSMSG_CANT_OPEN,
                            script->filename, strerror(errno));
            return JS_FALSE;
        }

        pc = script->code;
        end = pc + script->length;

        /* burn the leading lines */
        line2 = JS_PCToLineNumber(cx, script, pc);
        for (line1 = 0; line1 < line2 - 1; line1++)
            fgets(linebuf, LINE_BUF_LEN, file);

        bupline = 0;
        while (pc < end) {
            line2 = JS_PCToLineNumber(cx, script, pc);

            if (line2 < line1) {
                if (bupline != line2) {
                    bupline = line2;
                    fprintf(gOutFile, "%s %3u: BACKUP\n", sep, line2);
                }
            } else {
                if (bupline && line1 == line2)
                    fprintf(gOutFile, "%s %3u: RESTORE\n", sep, line2);
                bupline = 0;
                while (line1 < line2) {
                    if (!fgets(linebuf, LINE_BUF_LEN, file)) {
                        JS_ReportErrorNumber(cx, my_GetErrorMessage, NULL,
                                             JSSMSG_UNEXPECTED_EOF,
                                             script->filename);
                        goto bail;
                    }
                    line1++;
                    fprintf(gOutFile, "%s %3u: %s", sep, line1, linebuf);
                }
            }

            len = js_Disassemble1(cx, script, pc,
                                  PTRDIFF(pc, script->code, jsbytecode),
                                  JS_TRUE, stdout);
            if (!len)
                return JS_FALSE;
            pc += len;
        }

      bail:
        fclose(file);
    }
    return JS_TRUE;
#undef LINE_BUF_LEN
}

static JSBool
Tracing(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval)
{
    JSBool bval;
    JSString *str;

    if (argc == 0) {
        *rval = BOOLEAN_TO_JSVAL(cx->tracefp != 0);
        return JS_TRUE;
    }

    switch (JS_TypeOfValue(cx, argv[0])) {
      case JSTYPE_NUMBER:
        bval = JSVAL_IS_INT(argv[0])
               ? JSVAL_TO_INT(argv[0])
               : (jsint) *JSVAL_TO_DOUBLE(argv[0]);
        break;
      case JSTYPE_BOOLEAN:
        bval = JSVAL_TO_BOOLEAN(argv[0]);
        break;
      default:
        str = JS_ValueToString(cx, argv[0]);
        if (!str)
            return JS_FALSE;
        fprintf(gErrFile, "tracing: illegal argument %s\n",
                JS_GetStringBytes(str));
        return JS_TRUE;
    }
    cx->tracefp = bval ? stderr : NULL;
    return JS_TRUE;
}

typedef struct DumpAtomArgs {
    JSContext   *cx;
    FILE        *fp;
} DumpAtomArgs;

static int
DumpAtom(JSHashEntry *he, int i, void *arg)
{
    DumpAtomArgs *args = (DumpAtomArgs *)arg;
    FILE *fp = args->fp;
    JSAtom *atom = (JSAtom *)he;

    fprintf(fp, "%3d %08x %5lu ",
            i, (uintN)he->keyHash, (unsigned long)atom->number);
    if (ATOM_IS_STRING(atom))
        fprintf(fp, "\"%s\"\n", js_AtomToPrintableString(args->cx, atom));
    else if (ATOM_IS_INT(atom))
        fprintf(fp, "%ld\n", (long)ATOM_TO_INT(atom));
    else
        fprintf(fp, "%.16g\n", *ATOM_TO_DOUBLE(atom));
    return HT_ENUMERATE_NEXT;
}

static void
DumpScope(JSContext *cx, JSObject *obj, FILE *fp)
{
    uintN i;
    JSScope *scope;
    JSScopeProperty *sprop;

    i = 0;
    scope = OBJ_SCOPE(obj);
    for (sprop = SCOPE_LAST_PROP(scope); sprop; sprop = sprop->parent) {
        if (SCOPE_HAD_MIDDLE_DELETE(scope) && !SCOPE_HAS_PROPERTY(scope, sprop))
            continue;
        fprintf(fp, "%3u %p", i, (void *)sprop);
        if (JSID_IS_INT(sprop->id)) {
            fprintf(fp, " [%ld]", (long)JSVAL_TO_INT(sprop->id));
        } else if (JSID_IS_ATOM(sprop->id)) {
            JSAtom *atom = JSID_TO_ATOM(sprop->id);
            fprintf(fp, " \"%s\"", js_AtomToPrintableString(cx, atom));
        } else {
            jsval v = OBJECT_TO_JSVAL(JSID_TO_OBJECT(sprop->id));
            fprintf(fp, " \"%s\"", js_ValueToPrintableString(cx, v));
        }

#define DUMP_ATTR(name) if (sprop->attrs & JSPROP_##name) fputs(" " #name, fp)
        DUMP_ATTR(ENUMERATE);
        DUMP_ATTR(READONLY);
        DUMP_ATTR(PERMANENT);
        DUMP_ATTR(EXPORTED);
        DUMP_ATTR(GETTER);
        DUMP_ATTR(SETTER);
#undef  DUMP_ATTR

        fprintf(fp, " slot %lu flags %x shortid %d\n",
                (unsigned long)sprop->slot, sprop->flags, sprop->shortid);
    }
}

static JSBool
DumpStats(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval)
{
    uintN i;
    JSString *str;
    const char *bytes;
    JSAtom *atom;
    JSObject *obj2;
    JSProperty *prop;
    jsval value;

    for (i = 0; i < argc; i++) {
        str = JS_ValueToString(cx, argv[i]);
        if (!str)
            return JS_FALSE;
        bytes = JS_GetStringBytes(str);
        if (strcmp(bytes, "arena") == 0) {
#ifdef JS_ARENAMETER
            JS_DumpArenaStats(stdout);
#endif
        } else if (strcmp(bytes, "atom") == 0) {
            DumpAtomArgs args;

            fprintf(gOutFile, "\natom table contents:\n");
            args.cx = cx;
            args.fp = stdout;
            JS_HashTableEnumerateEntries(cx->runtime->atomState.table,
                                         DumpAtom,
                                         &args);
#ifdef HASHMETER
            JS_HashTableDumpMeter(cx->runtime->atomState.table,
                                  DumpAtom,
                                  stdout);
#endif
        } else if (strcmp(bytes, "global") == 0) {
            DumpScope(cx, cx->globalObject, stdout);
        } else {
            atom = js_Atomize(cx, bytes, JS_GetStringLength(str), 0);
            if (!atom)
                return JS_FALSE;
            if (!js_FindProperty(cx, ATOM_TO_JSID(atom), &obj, &obj2, &prop))
                return JS_FALSE;
            if (prop) {
                OBJ_DROP_PROPERTY(cx, obj2, prop);
                if (!OBJ_GET_PROPERTY(cx, obj, ATOM_TO_JSID(atom), &value))
                    return JS_FALSE;
            }
            if (!prop || !JSVAL_IS_OBJECT(value)) {
                fprintf(gErrFile, "js: invalid stats argument %s\n",
                        bytes);
                continue;
            }
            obj = JSVAL_TO_OBJECT(value);
            if (obj)
                DumpScope(cx, obj, stdout);
        }
    }
    return JS_TRUE;
}

#endif /* DEBUG */

#ifdef TEST_EXPORT
static JSBool
DoExport(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval)
{
    JSAtom *atom;
    JSObject *obj2;
    JSProperty *prop;
    JSBool ok;
    uintN attrs;

    if (argc != 2) {
        JS_ReportErrorNumber(cx, my_GetErrorMessage, NULL, JSSMSG_DOEXP_USAGE);
        return JS_FALSE;
    }
    if (!JS_ValueToObject(cx, argv[0], &obj))
        return JS_FALSE;
    argv[0] = OBJECT_TO_JSVAL(obj);
    atom = js_ValueToStringAtom(cx, argv[1]);
    if (!atom)
        return JS_FALSE;
    if (!OBJ_LOOKUP_PROPERTY(cx, obj, ATOM_TO_JSID(atom), &obj2, &prop))
        return JS_FALSE;
    if (!prop) {
        ok = OBJ_DEFINE_PROPERTY(cx, obj, id, JSVAL_VOID, NULL, NULL,
                                 JSPROP_EXPORTED, NULL);
    } else {
        ok = OBJ_GET_ATTRIBUTES(cx, obj, ATOM_TO_JSID(atom), prop, &attrs);
        if (ok) {
            attrs |= JSPROP_EXPORTED;
            ok = OBJ_SET_ATTRIBUTES(cx, obj, ATOM_TO_JSID(atom), prop, &attrs);
        }
        OBJ_DROP_PROPERTY(cx, obj2, prop);
    }
    return ok;
}
#endif

#ifdef TEST_CVTARGS
#include <ctype.h>

static const char *
EscapeWideString(jschar *w)
{
    static char enuf[80];
    static char hex[] = "0123456789abcdef";
    jschar u;
    unsigned char b, c;
    int i, j;

    if (!w)
        return "";
    for (i = j = 0; i < sizeof enuf - 1; i++, j++) {
        u = w[j];
        if (u == 0)
            break;
        b = (unsigned char)(u >> 8);
        c = (unsigned char)(u);
        if (b) {
            if (i >= sizeof enuf - 6)
                break;
            enuf[i++] = '\\';
            enuf[i++] = 'u';
            enuf[i++] = hex[b >> 4];
            enuf[i++] = hex[b & 15];
            enuf[i++] = hex[c >> 4];
            enuf[i] = hex[c & 15];
        } else if (!isprint(c)) {
            if (i >= sizeof enuf - 4)
                break;
            enuf[i++] = '\\';
            enuf[i++] = 'x';
            enuf[i++] = hex[c >> 4];
            enuf[i] = hex[c & 15];
        } else {
            enuf[i] = (char)c;
        }
    }
    enuf[i] = 0;
    return enuf;
}

#include <stdarg.h>

static JSBool
ZZ_formatter(JSContext *cx, const char *format, JSBool fromJS, jsval **vpp,
             va_list *app)
{
    jsval *vp;
    va_list ap;
    jsdouble re, im;

    printf("entering ZZ_formatter");
    vp = *vpp;
    ap = *app;
    if (fromJS) {
        if (!JS_ValueToNumber(cx, vp[0], &re))
            return JS_FALSE;
        if (!JS_ValueToNumber(cx, vp[1], &im))
            return JS_FALSE;
        *va_arg(ap, jsdouble *) = re;
        *va_arg(ap, jsdouble *) = im;
    } else {
        re = va_arg(ap, jsdouble);
        im = va_arg(ap, jsdouble);
        if (!JS_NewNumberValue(cx, re, &vp[0]))
            return JS_FALSE;
        if (!JS_NewNumberValue(cx, im, &vp[1]))
            return JS_FALSE;
    }
    *vpp = vp + 2;
    *app = ap;
    printf("leaving ZZ_formatter");
    return JS_TRUE;
}

static JSBool
ConvertArgs(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval)
{
    JSBool b = JS_FALSE;
    jschar c = 0;
    int32 i = 0, j = 0;
    uint32 u = 0;
    jsdouble d = 0, I = 0, re = 0, im = 0;
    char *s = NULL;
    JSString *str = NULL;
    jschar *w = NULL;
    JSObject *obj2 = NULL;
    JSFunction *fun = NULL;
    jsval v = JSVAL_VOID;
    JSBool ok;

    if (!JS_AddArgumentFormatter(cx, "ZZ", ZZ_formatter))
        return JS_FALSE;;
    ok = JS_ConvertArguments(cx, argc, argv, "b/ciujdIsSWofvZZ*",
                             &b, &c, &i, &u, &j, &d, &I, &s, &str, &w, &obj2,
                             &fun, &v, &re, &im);
    JS_RemoveArgumentFormatter(cx, "ZZ");
    if (!ok)
        return JS_FALSE;
    fprintf(gOutFile,
            "b %u, c %x (%c), i %ld, u %lu, j %ld\n",
            b, c, (char)c, i, u, j);
    fprintf(gOutFile,
            "d %g, I %g, s %s, S %s, W %s, obj %s, fun %s\n"
            "v %s, re %g, im %g\n",
            d, I, s, str ? JS_GetStringBytes(str) : "", EscapeWideString(w),
            JS_GetStringBytes(JS_ValueToString(cx, OBJECT_TO_JSVAL(obj2))),
            fun ? JS_GetStringBytes(JS_DecompileFunction(cx, fun, 4)) : "",
            JS_GetStringBytes(JS_ValueToString(cx, v)), re, im);
    return JS_TRUE;
}
#endif

static JSBool
BuildDate(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval)
{
    char version[20] = "\n";
#if JS_VERSION < 150
    sprintf(version, " for version %d\n", JS_VERSION);
#endif
    fprintf(gOutFile, "built on %s at %s%s", __DATE__, __TIME__, version);
    return JS_TRUE;
}

static JSBool
Clear(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval)
{
    if (argc != 0 && !JS_ValueToObject(cx, argv[0], &obj))
        return JS_FALSE;
    JS_ClearScope(cx, obj);
    return JS_TRUE;
}

static JSBool
Intern(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval)
{
    JSString *str;

    str = JS_ValueToString(cx, argv[0]);
    if (!str)
        return JS_FALSE;
    if (!JS_InternUCStringN(cx, JS_GetStringChars(str),
                                JS_GetStringLength(str))) {
        return JS_FALSE;
    }
    return JS_TRUE;
}

static JSBool
Clone(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval)
{
    JSFunction *fun;
    JSObject *funobj, *parent, *clone;

    fun = JS_ValueToFunction(cx, argv[0]);
    if (!fun)
        return JS_FALSE;
    funobj = JS_GetFunctionObject(fun);
    if (argc > 1) {
        if (!JS_ValueToObject(cx, argv[1], &parent))
            return JS_FALSE;
    } else {
        parent = JS_GetParent(cx, funobj);
    }
    clone = JS_CloneFunctionObject(cx, funobj, parent);
    if (!clone)
        return JS_FALSE;
    *rval = OBJECT_TO_JSVAL(clone);
    return JS_TRUE;
}

static JSBool
Seal(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval)
{
    JSObject *target;
    JSBool deep = JS_FALSE;

    if (!JS_ConvertArguments(cx, argc, argv, "o/b", &target, &deep))
        return JS_FALSE;
    if (!target)
        return JS_TRUE;
    return JS_SealObject(cx, target, deep);
}

static JSBool
GetPDA(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval)
{
    JSObject *vobj, *aobj, *pdobj;
    JSBool ok;
    JSPropertyDescArray pda;
    JSPropertyDesc *pd;
    uint32 i;
    jsval v;

    if (!JS_ValueToObject(cx, argv[0], &vobj))
        return JS_FALSE;
    if (!vobj)
        return JS_TRUE;

    aobj = JS_NewArrayObject(cx, 0, NULL);
    if (!aobj)
        return JS_FALSE;
    *rval = OBJECT_TO_JSVAL(aobj);

    ok = JS_GetPropertyDescArray(cx, vobj, &pda);
    if (!ok)
        return JS_FALSE;
    pd = pda.array;
    for (i = 0; i < pda.length; i++) {
        pdobj = JS_NewObject(cx, NULL, NULL, NULL);
        if (!pdobj) {
            ok = JS_FALSE;
            break;
        }

        ok = JS_SetProperty(cx, pdobj, "id", &pd->id) &&
             JS_SetProperty(cx, pdobj, "value", &pd->value) &&
             (v = INT_TO_JSVAL(pd->flags),
              JS_SetProperty(cx, pdobj, "flags", &v)) &&
             (v = INT_TO_JSVAL(pd->slot),
              JS_SetProperty(cx, pdobj, "slot", &v)) &&
             JS_SetProperty(cx, pdobj, "alias", &pd->alias);
        if (!ok)
            break;

        v = OBJECT_TO_JSVAL(pdobj);
        ok = JS_SetElement(cx, aobj, i, &v);
        if (!ok)
            break;
    }
    JS_PutPropertyDescArray(cx, &pda);
    return ok;
}

static JSBool
GetSLX(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval)
{
    JSScript *script;

    script = ValueToScript(cx, argv[0]);
    if (!script)
        return JS_FALSE;
    *rval = INT_TO_JSVAL(js_GetScriptLineExtent(script));
    return JS_TRUE;
}

static JSBool
ToInt32(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval)
{
    int32 i;

    if (!JS_ValueToInt32(cx, argv[0], &i))
        return JS_FALSE;
    return JS_NewNumberValue(cx, i, rval);
}

static JSBool
StringsAreUtf8(JSContext *cx, JSObject *obj, uintN argc, jsval *argv,
               jsval *rval)
{
    *rval = JS_CStringsAreUTF8() ? JSVAL_TRUE : JSVAL_FALSE;
    return JS_TRUE;
}

static const char* badUtf8 = "...\xC0...";
static const char* bigUtf8 = "...\xFB\xBF\xBF\xBF\xBF...";
static const jschar badSurrogate[] = { 'A', 'B', 'C', 0xDEEE, 'D', 'E', 0 };

static JSBool
TestUtf8(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval)
{
    intN mode = 1;
    jschar chars[20];
    size_t charsLength = 5;
    char bytes[20];
    size_t bytesLength = 20;
    if (argc && !JS_ValueToInt32(cx, *argv, &mode))
        return JS_FALSE;

    /* The following throw errors if compiled with UTF-8. */
    switch (mode) {
      /* mode 1: malformed UTF-8 string. */
      case 1:
        JS_NewStringCopyZ(cx, badUtf8);
        break;
      /* mode 2: big UTF-8 character. */
      case 2:
        JS_NewStringCopyZ(cx, bigUtf8);
        break;
      /* mode 3: bad surrogate character. */
      case 3:
        JS_EncodeCharacters(cx, badSurrogate, 6, bytes, &bytesLength);
        break;
      /* mode 4: use a too small buffer. */
      case 4:
        JS_DecodeBytes(cx, "1234567890", 10, chars, &charsLength);
        break;
      default:
        JS_ReportError(cx, "invalid mode parameter");
        return JS_FALSE;
    }
    return !JS_IsExceptionPending (cx);
}

static JSBool
ThrowError(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval)
{
    JS_ReportError(cx, "This is an error");
    return JS_FALSE;
}

#define LAZY_STANDARD_CLASSES

/* A class for easily testing the inner/outer object callbacks. */
typedef struct ComplexObject {
    JSBool isInner;
    JSObject *inner;
    JSObject *outer;
} ComplexObject;

static JSObject *
split_create_outer(JSContext *cx);

static JSObject *
split_create_inner(JSContext *cx, JSObject *outer);

static ComplexObject *
split_get_private(JSContext *cx, JSObject *obj);

JS_STATIC_DLL_CALLBACK(JSBool)
split_addProperty(JSContext *cx, JSObject *obj, jsval id, jsval *vp)
{
    ComplexObject *cpx;
    jsid asId;

    cpx = split_get_private(cx, obj);
    if (!cpx)
        return JS_TRUE;
    if (!cpx->isInner && cpx->inner) {
        /* Make sure to define this property on the inner object. */
        if (!JS_ValueToId(cx, *vp, &asId))
            return JS_FALSE;
        return OBJ_DEFINE_PROPERTY(cx, cpx->inner, asId, *vp, NULL, NULL,
                                   JSPROP_ENUMERATE, NULL);
    }
    return JS_TRUE;
}

JS_STATIC_DLL_CALLBACK(JSBool)
split_getProperty(JSContext *cx, JSObject *obj, jsval id, jsval *vp)
{
    ComplexObject *cpx;

    cpx = split_get_private(cx, obj);
    if (!cpx)
        return JS_TRUE;
    if (!cpx->isInner && cpx->inner) {
        if (JSVAL_IS_STRING(id)) {
            JSString *str;

            str = JSVAL_TO_STRING(id);
            return JS_GetUCProperty(cx, cpx->inner, JS_GetStringChars(str),
                                    JS_GetStringLength(str), vp);
        }
        if (JSVAL_IS_INT(id))
            return JS_GetElement(cx, cpx->inner, JSVAL_TO_INT(id), vp);
        return JS_TRUE;
    }

    return JS_TRUE;
}

JS_STATIC_DLL_CALLBACK(JSBool)
split_setProperty(JSContext *cx, JSObject *obj, jsval id, jsval *vp)
{
    ComplexObject *cpx;

    cpx = split_get_private(cx, obj);
    if (!cpx)
        return JS_TRUE;
    if (!cpx->isInner && cpx->inner) {
        if (JSVAL_IS_STRING(id)) {
            JSString *str;

            str = JSVAL_TO_STRING(id);
            return JS_SetUCProperty(cx, cpx->inner, JS_GetStringChars(str),
                                    JS_GetStringLength(str), vp);
        }
        if (JSVAL_IS_INT(id))
            return JS_SetElement(cx, cpx->inner, JSVAL_TO_INT(id), vp);
        return JS_TRUE;
    }

    return JS_TRUE;
}

JS_STATIC_DLL_CALLBACK(JSBool)
split_delProperty(JSContext *cx, JSObject *obj, jsval id, jsval *vp)
{
    ComplexObject *cpx;
    jsid asId;

    cpx = split_get_private(cx, obj);
    if (!cpx)
        return JS_TRUE;
    if (!cpx->isInner && cpx->inner) {
        /* Make sure to define this property on the inner object. */
        if (!JS_ValueToId(cx, *vp, &asId))
            return JS_FALSE;
        return OBJ_DELETE_PROPERTY(cx, cpx->inner, asId, vp);
    }
    return JS_TRUE;
}

JS_STATIC_DLL_CALLBACK(JSBool)
split_enumerate(JSContext *cx, JSObject *obj, JSIterateOp enum_op,
                  jsval *statep, jsid *idp)
{
    ComplexObject *cpx;
    JSObject *iterator;

    switch (enum_op) {
      case JSENUMERATE_INIT:
        cpx = JS_GetPrivate(cx, obj);

        if (!cpx->isInner && cpx->inner)
            obj = cpx->inner;

        iterator = JS_NewPropertyIterator(cx, obj);
        if (!iterator)
            return JS_FALSE;

        *statep = OBJECT_TO_JSVAL(iterator);
        if (idp)
            *idp = JSVAL_ZERO;
        break;

      case JSENUMERATE_NEXT:
        iterator = (JSObject*)JSVAL_TO_OBJECT(*statep);
        if (!JS_NextProperty(cx, iterator, idp))
            return JS_FALSE;

        if (*idp != JSVAL_VOID)
            break;
        /* Fall through. */

      case JSENUMERATE_DESTROY:
        /* Let GC at our iterator object. */
        *statep = JSVAL_NULL;
        break;
    }

    return JS_TRUE;
}

JS_STATIC_DLL_CALLBACK(JSBool)
split_resolve(JSContext *cx, JSObject *obj, jsval id, uintN flags,
                JSObject **objp)
{
    ComplexObject *cpx;

    cpx = split_get_private(cx, obj);
    if (!cpx)
        return JS_TRUE;
    if (!cpx->isInner && cpx->inner) {
        jsid asId;
        JSProperty *prop;

        if (!JS_ValueToId(cx, id, &asId))
            return JS_FALSE;

        if (!OBJ_LOOKUP_PROPERTY(cx, cpx->inner, asId, objp, &prop))
            return JS_FALSE;
        if (prop)
            OBJ_DROP_PROPERTY(cx, cpx->inner, prop);

        return JS_TRUE;
    }

#ifdef LAZY_STANDARD_CLASSES
    if (!(flags & JSRESOLVE_ASSIGNING)) {
        JSBool resolved;

        if (!JS_ResolveStandardClass(cx, obj, id, &resolved))
            return JS_FALSE;

        if (resolved) {
            *objp = obj;
            return JS_TRUE;
        }
    }
#endif

    /* XXX For additional realism, let's resolve some random property here. */
    return JS_TRUE;
}

JS_STATIC_DLL_CALLBACK(void)
split_finalize(JSContext *cx, JSObject *obj)
{
    JS_free(cx, JS_GetPrivate(cx, obj));
}

JS_STATIC_DLL_CALLBACK(uint32)
split_mark(JSContext *cx, JSObject *obj, void *arg)
{
    ComplexObject *cpx;

    cpx = JS_GetPrivate(cx, obj);

    if (!cpx->isInner && cpx->inner) {
        /* Mark the inner object. */
        JS_MarkGCThing(cx, cpx->inner, "ComplexObject.inner", arg);
    }

    return 0;
}

JS_STATIC_DLL_CALLBACK(JSObject *)
split_outerObject(JSContext *cx, JSObject *obj)
{
    ComplexObject *cpx;

    cpx = JS_GetPrivate(cx, obj);
    return cpx->isInner ? cpx->outer : obj;
}

JS_STATIC_DLL_CALLBACK(JSObject *)
split_innerObject(JSContext *cx, JSObject *obj)
{
    ComplexObject *cpx;

    cpx = JS_GetPrivate(cx, obj);
    return !cpx->isInner ? cpx->inner : obj;
}

static JSExtendedClass split_global_class = {
    {"split_global",
    JSCLASS_NEW_RESOLVE | JSCLASS_HAS_PRIVATE | JSCLASS_IS_EXTENDED,
    split_addProperty, split_delProperty,
    split_getProperty, split_setProperty,
    (JSEnumerateOp)split_enumerate,
    (JSResolveOp)split_resolve,
    JS_ConvertStub, split_finalize,
    NULL, NULL, NULL, NULL, NULL, NULL,
    split_mark, NULL},
    NULL, split_outerObject, split_innerObject,
    NULL, NULL, NULL, NULL, NULL
};

JSObject *
split_create_outer(JSContext *cx)
{
    ComplexObject *cpx;
    JSObject *obj;

    cpx = JS_malloc(cx, sizeof *obj);
    if (!cpx)
        return NULL;
    cpx->outer = NULL;
    cpx->inner = NULL;
    cpx->isInner = JS_FALSE;

    obj = JS_NewObject(cx, &split_global_class.base, NULL, NULL);
    if (!obj) {
        JS_free(cx, cpx);
        return NULL;
    }

    JS_ASSERT(!JS_GetParent(cx, obj));
    if (!JS_SetPrivate(cx, obj, cpx)) {
        JS_free(cx, cpx);
        return NULL;
    }

    return obj;
}

static JSObject *
split_create_inner(JSContext *cx, JSObject *outer)
{
    ComplexObject *cpx, *outercpx;
    JSObject *obj;

    JS_ASSERT(JS_GET_CLASS(cx, outer) == &split_global_class.base);

    cpx = JS_malloc(cx, sizeof *cpx);
    if (!cpx)
        return NULL;
    cpx->outer = outer;
    cpx->inner = NULL;
    cpx->isInner = JS_TRUE;

    obj = JS_NewObject(cx, &split_global_class.base, NULL, NULL);
    if (!obj || !JS_SetParent(cx, obj, NULL) || !JS_SetPrivate(cx, obj, cpx)) {
        JS_free(cx, cpx);
        return NULL;
    }

    outercpx = JS_GetPrivate(cx, outer);
    outercpx->inner = obj;

    return obj;
}

static ComplexObject *
split_get_private(JSContext *cx, JSObject *obj)
{
    do {
        if (JS_GET_CLASS(cx, obj) == &split_global_class.base)
            return JS_GetPrivate(cx, obj);
        obj = JS_GetParent(cx, obj);
    } while (obj);

    return NULL;
}

static JSBool
sandbox_enumerate(JSContext *cx, JSObject *obj)
{
    jsval v;
    JSBool b;

    if (!JS_GetProperty(cx, obj, "lazy", &v) || !JS_ValueToBoolean(cx, v, &b))
        return JS_FALSE;
    return !b || JS_EnumerateStandardClasses(cx, obj);
}

static JSBool
sandbox_resolve(JSContext *cx, JSObject *obj, jsval id, uintN flags,
                JSObject **objp)
{
    jsval v;
    JSBool b, resolved;

    if (!JS_GetProperty(cx, obj, "lazy", &v) || !JS_ValueToBoolean(cx, v, &b))
        return JS_FALSE;
    if (b && (flags & JSRESOLVE_ASSIGNING) == 0) {
        if (!JS_ResolveStandardClass(cx, obj, id, &resolved))
            return JS_FALSE;
        if (resolved) {
            *objp = obj;
            return JS_TRUE;
        }
    }
    *objp = NULL;
    return JS_TRUE;
}

static JSClass sandbox_class = {
    "sandbox",
    JSCLASS_NEW_RESOLVE,
    JS_PropertyStub,   JS_PropertyStub,
    JS_PropertyStub,   JS_PropertyStub,
    sandbox_enumerate, (JSResolveOp)sandbox_resolve,
    JS_ConvertStub,    JS_FinalizeStub,
    JSCLASS_NO_OPTIONAL_MEMBERS
};

static JSBool
EvalInContext(JSContext *cx, JSObject *obj, uintN argc, jsval *argv,
              jsval *rval)
{
    JSString *str;
    JSObject *sobj;
    JSContext *scx;
    const jschar *src;
    size_t srclen;
    JSBool lazy, ok;
    jsval v;
    JSStackFrame *fp;

    sobj = NULL;
    if (!JS_ConvertArguments(cx, argc, argv, "S / o", &str, &sobj))
        return JS_FALSE;

    scx = JS_NewContext(JS_GetRuntime(cx), gStackChunkSize);
    if (!scx) {
        JS_ReportOutOfMemory(cx);
        return JS_FALSE;
    }

    src = JS_GetStringChars(str);
    srclen = JS_GetStringLength(str);
    lazy = JS_FALSE;
    if (srclen == 4 &&
        src[0] == 'l' && src[1] == 'a' && src[2] == 'z' && src[3] == 'y') {
        lazy = JS_TRUE;
        srclen = 0;
    }

    if (!sobj) {
        sobj = JS_NewObject(scx, &sandbox_class, NULL, NULL);
        if (!sobj || (!lazy && !JS_InitStandardClasses(scx, sobj))) {
            ok = JS_FALSE;
            goto out;
        }
        v = BOOLEAN_TO_JSVAL(v);
        ok = JS_SetProperty(cx, sobj, "lazy", &v);
        if (!ok)
            goto out;
    }

    if (srclen == 0) {
        *rval = OBJECT_TO_JSVAL(sobj);
        ok = JS_TRUE;
    } else {
        fp = JS_GetScriptedCaller(cx, NULL);
        ok = JS_EvaluateUCScript(scx, sobj, src, srclen,
                                 fp->script->filename,
                                 JS_PCToLineNumber(cx, fp->script, fp->pc),
                                 rval);
    }

out:
    JS_DestroyContext(scx);
    return ok;
}

static JSFunctionSpec shell_functions[] = {
    {"version",         Version,        0,0,0},
    {"options",         Options,        0,0,0},
    {"load",            Load,           1,0,0},
    {"readline",        ReadLine,       0,0,0},
    {"print",           Print,          0,0,0},
    {"help",            Help,           0,0,0},
    {"quit",            Quit,           0,0,0},
    {"gc",              GC,             0,0,0},
    {"trap",            Trap,           3,0,0},
    {"untrap",          Untrap,         2,0,0},
    {"line2pc",         LineToPC,       0,0,0},
    {"pc2line",         PCToLine,       0,0,0},
    {"stringsAreUtf8",  StringsAreUtf8, 0,0,0},
    {"testUtf8",        TestUtf8,       1,0,0},
    {"throwError",      ThrowError,     0,0,0},
#ifdef DEBUG
    {"dis",             Disassemble,    1,0,0},
    {"dissrc",          DisassWithSrc,  1,0,0},
    {"notes",           Notes,          1,0,0},
    {"tracing",         Tracing,        0,0,0},
    {"stats",           DumpStats,      1,0,0},
#endif
#ifdef TEST_EXPORT
    {"xport",           DoExport,       2,0,0},
#endif
#ifdef TEST_CVTARGS
    {"cvtargs",         ConvertArgs,    0,0,12},
#endif
    {"build",           BuildDate,      0,0,0},
    {"clear",           Clear,          0,0,0},
    {"intern",          Intern,         1,0,0},
    {"clone",           Clone,          1,0,0},
    {"seal",            Seal,           1,0,1},
    {"getpda",          GetPDA,         1,0,0},
    {"getslx",          GetSLX,         1,0,0},
    {"toint32",         ToInt32,        1,0,0},
    {"evalcx",          EvalInContext,  1,0,0},
    {NULL,NULL,0,0,0}
};

/* NOTE: These must be kept in sync with the above. */

static char *shell_help_messages[] = {
    "version([number])      Get or set JavaScript version number",
    "options([option ...])  Get or toggle JavaScript options",
    "load(['foo.js' ...])   Load files named by string arguments",
    "readline()             Read a single line from stdin",
    "print([exp ...])       Evaluate and print expressions",
    "help([name ...])       Display usage and help messages",
    "quit()                 Quit the shell",
    "gc()                   Run the garbage collector",
    "trap([fun, [pc,]] exp) Trap bytecode execution",
    "untrap(fun[, pc])      Remove a trap",
    "line2pc([fun,] line)   Map line number to PC",
    "pc2line(fun[, pc])     Map PC to line number",
    "stringsAreUTF8()       Check if strings are UTF-8 encoded",
    "testUTF8(mode)         Perform UTF-8 tests (modes are 1 to 4)",
    "throwError()           Throw an error from JS_ReportError",
#ifdef DEBUG
    "dis([fun])             Disassemble functions into bytecodes",
    "dissrc([fun])          Disassemble functions with source lines",
    "notes([fun])           Show source notes for functions",
    "tracing([toggle])      Turn tracing on or off",
    "stats([string ...])    Dump 'arena', 'atom', 'global' stats",
#endif
#ifdef TEST_EXPORT
    "xport(obj, id)         Export identified property from object",
#endif
#ifdef TEST_CVTARGS
    "cvtargs(b, c, ...)     Test JS_ConvertArguments",
#endif
    "build()                Show build date and time",
    "clear([obj])           Clear properties of object",
    "intern(str)            Internalize str in the atom table",
    "clone(fun[, scope])    Clone function object",
    "seal(obj[, deep])      Seal object, or object graph if deep",
    "getpda(obj)            Get the property descriptors for obj",
    "getslx(obj)            Get script line extent",
    "toint32(n)             Testing hook for JS_ValueToInt32",
    "evalcx(s[, o])         Evaluate s in optional sandbox object o\n"
    "    if (s == '' && !o) return new o with eager standard classes\n"
    "    if (s == 'lazy' && !o) return new o with lazy standard classes",
    0
};

static void
ShowHelpHeader(void)
{
    fprintf(gOutFile, "%-14s %-22s %s\n", "Command", "Usage", "Description");
    fprintf(gOutFile, "%-14s %-22s %s\n", "=======", "=====", "===========");
}

static void
ShowHelpForCommand(uintN n)
{
    fprintf(gOutFile, "%-14.14s %s\n", shell_functions[n].name, shell_help_messages[n]);
}

static JSObject *
split_setup(JSContext *cx)
{
    JSObject *outer, *inner, *arguments;

    outer = split_create_outer(cx);
    if (!outer)
        return NULL;
    JS_SetGlobalObject(cx, outer);

    inner = split_create_inner(cx, outer);
    if (!inner)
        return NULL;

    if (!JS_DefineFunctions(cx, inner, shell_functions))
        return NULL;
    JS_ClearScope(cx, outer);

    /* Create a dummy arguments object. */
    arguments = JS_NewArrayObject(cx, 0, NULL);
    if (!arguments ||
        !JS_DefineProperty(cx, inner, "arguments", OBJECT_TO_JSVAL(arguments),
                           NULL, NULL, 0)) {
        return NULL;
    }

#ifndef LAZY_STANDARD_CLASSES
    if (!JS_InitStandardClasses(cx, inner))
        return NULL;
#endif

    return inner;
}

static JSBool
Help(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval)
{
    uintN i, j;
    int did_header, did_something;
    JSType type;
    JSFunction *fun;
    JSString *str;
    const char *bytes;

    fprintf(gOutFile, "%s\n", JS_GetImplementationVersion());
    if (argc == 0) {
        ShowHelpHeader();
        for (i = 0; shell_functions[i].name; i++)
            ShowHelpForCommand(i);
    } else {
        did_header = 0;
        for (i = 0; i < argc; i++) {
            did_something = 0;
            type = JS_TypeOfValue(cx, argv[i]);
            if (type == JSTYPE_FUNCTION) {
                fun = JS_ValueToFunction(cx, argv[i]);
                str = fun->atom ? ATOM_TO_STRING(fun->atom) : NULL;
            } else if (type == JSTYPE_STRING) {
                str = JSVAL_TO_STRING(argv[i]);
            } else {
                str = NULL;
            }
            if (str) {
                bytes = JS_GetStringBytes(str);
                for (j = 0; shell_functions[j].name; j++) {
                    if (!strcmp(bytes, shell_functions[j].name)) {
                        if (!did_header) {
                            did_header = 1;
                            ShowHelpHeader();
                        }
                        did_something = 1;
                        ShowHelpForCommand(j);
                        break;
                    }
                }
            }
            if (!did_something) {
                str = JS_ValueToString(cx, argv[i]);
                if (!str)
                    return JS_FALSE;
                fprintf(gErrFile, "Sorry, no help for %s\n",
                        JS_GetStringBytes(str));
            }
        }
    }
    return JS_TRUE;
}

/*
 * Define a JS object called "it".  Give it class operations that printf why
 * they're being called for tutorial purposes.
 */
enum its_tinyid {
    ITS_COLOR, ITS_HEIGHT, ITS_WIDTH, ITS_FUNNY, ITS_ARRAY, ITS_RDONLY
};

static JSPropertySpec its_props[] = {
    {"color",           ITS_COLOR,      JSPROP_ENUMERATE,       NULL, NULL},
    {"height",          ITS_HEIGHT,     JSPROP_ENUMERATE,       NULL, NULL},
    {"width",           ITS_WIDTH,      JSPROP_ENUMERATE,       NULL, NULL},
    {"funny",           ITS_FUNNY,      JSPROP_ENUMERATE,       NULL, NULL},
    {"array",           ITS_ARRAY,      JSPROP_ENUMERATE,       NULL, NULL},
    {"rdonly",          ITS_RDONLY,     JSPROP_READONLY,        NULL, NULL},
    {NULL,0,0,NULL,NULL}
};

static JSBool
its_item(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval)
{
    *rval = OBJECT_TO_JSVAL(obj);
    if (argc != 0)
        JS_SetCallReturnValue2(cx, argv[0]);
    return JS_TRUE;
}

static JSBool
its_bindMethod(JSContext *cx, JSObject *obj, uintN argc, jsval *argv,
               jsval *rval)
{
    char *name;
    JSObject *method;

    if (!JS_ConvertArguments(cx, argc, argv, "so", &name, &method))
        return JS_FALSE;

    *rval = OBJECT_TO_JSVAL(method);

    if (JS_TypeOfValue(cx, *rval) != JSTYPE_FUNCTION) {
        JSString *valstr = JS_ValueToString(cx, *rval);
        if (valstr) {
            JS_ReportError(cx, "can't bind method %s to non-callable object %s",
                           name, JS_GetStringBytes(valstr));
        }
        return JS_FALSE;
    }

    if (!JS_DefineProperty(cx, obj, name, *rval, NULL, NULL, JSPROP_ENUMERATE))
        return JS_FALSE;

    return JS_SetParent(cx, method, obj);
}

static JSFunctionSpec its_methods[] = {
    {"item",            its_item,       0,0,0},
    {"bindMethod",      its_bindMethod, 2,0,0},
    {NULL,NULL,0,0,0}
};

#ifdef JSD_LOWLEVEL_SOURCE
/*
 * This facilitates sending source to JSD (the debugger system) in the shell
 * where the source is loaded using the JSFILE hack in jsscan. The function
 * below is used as a callback for the jsdbgapi JS_SetSourceHandler hook.
 * A more normal embedding (e.g. mozilla) loads source itself and can send
 * source directly to JSD without using this hook scheme.
 */
static void
SendSourceToJSDebugger(const char *filename, uintN lineno,
                       jschar *str, size_t length,
                       void **listenerTSData, JSDContext* jsdc)
{
    JSDSourceText *jsdsrc = (JSDSourceText *) *listenerTSData;

    if (!jsdsrc) {
        if (!filename)
            filename = "typein";
        if (1 == lineno) {
            jsdsrc = JSD_NewSourceText(jsdc, filename);
        } else {
            jsdsrc = JSD_FindSourceForURL(jsdc, filename);
            if (jsdsrc && JSD_SOURCE_PARTIAL !=
                JSD_GetSourceStatus(jsdc, jsdsrc)) {
                jsdsrc = NULL;
            }
        }
    }
    if (jsdsrc) {
        jsdsrc = JSD_AppendUCSourceText(jsdc,jsdsrc, str, length,
                                        JSD_SOURCE_PARTIAL);
    }
    *listenerTSData = jsdsrc;
}
#endif /* JSD_LOWLEVEL_SOURCE */

static JSBool its_noisy;    /* whether to be noisy when finalizing it */

static JSBool
its_addProperty(JSContext *cx, JSObject *obj, jsval id, jsval *vp)
{
    if (its_noisy) {
        fprintf(gOutFile, "adding its property %s,",
               JS_GetStringBytes(JS_ValueToString(cx, id)));
        fprintf(gOutFile, " initial value %s\n",
               JS_GetStringBytes(JS_ValueToString(cx, *vp)));
    }
    return JS_TRUE;
}

static JSBool
its_delProperty(JSContext *cx, JSObject *obj, jsval id, jsval *vp)
{
    if (its_noisy) {
        fprintf(gOutFile, "deleting its property %s,",
               JS_GetStringBytes(JS_ValueToString(cx, id)));
        fprintf(gOutFile, " current value %s\n",
               JS_GetStringBytes(JS_ValueToString(cx, *vp)));
    }
    return JS_TRUE;
}

static JSBool
its_getProperty(JSContext *cx, JSObject *obj, jsval id, jsval *vp)
{
    if (its_noisy) {
        fprintf(gOutFile, "getting its property %s,",
               JS_GetStringBytes(JS_ValueToString(cx, id)));
        fprintf(gOutFile, " current value %s\n",
               JS_GetStringBytes(JS_ValueToString(cx, *vp)));
    }
    return JS_TRUE;
}

static JSBool
its_setProperty(JSContext *cx, JSObject *obj, jsval id, jsval *vp)
{
    if (its_noisy) {
        fprintf(gOutFile, "setting its property %s,",
               JS_GetStringBytes(JS_ValueToString(cx, id)));
        fprintf(gOutFile, " new value %s\n",
               JS_GetStringBytes(JS_ValueToString(cx, *vp)));
    }
    if (JSVAL_IS_STRING(id) &&
        !strcmp(JS_GetStringBytes(JSVAL_TO_STRING(id)), "noisy")) {
        return JS_ValueToBoolean(cx, *vp, &its_noisy);
    }
    return JS_TRUE;
}

static JSBool
its_enumerate(JSContext *cx, JSObject *obj)
{
    if (its_noisy)
        fprintf(gOutFile, "enumerate its properties\n");
    return JS_TRUE;
}

static JSBool
its_resolve(JSContext *cx, JSObject *obj, jsval id, uintN flags,
            JSObject **objp)
{
    if (its_noisy) {
        fprintf(gOutFile, "resolving its property %s, flags {%s,%s,%s}\n",
               JS_GetStringBytes(JS_ValueToString(cx, id)),
               (flags & JSRESOLVE_QUALIFIED) ? "qualified" : "",
               (flags & JSRESOLVE_ASSIGNING) ? "assigning" : "",
               (flags & JSRESOLVE_DETECTING) ? "detecting" : "");
    }
    return JS_TRUE;
}

static JSBool
its_convert(JSContext *cx, JSObject *obj, JSType type, jsval *vp)
{
    if (its_noisy)
        fprintf(gOutFile, "converting it to %s type\n", JS_GetTypeName(cx, type));
    return JS_TRUE;
}

static void
its_finalize(JSContext *cx, JSObject *obj)
{
    if (its_noisy)
        fprintf(gOutFile, "finalizing it\n");
}

static JSClass its_class = {
    "It", JSCLASS_NEW_RESOLVE,
    its_addProperty,  its_delProperty,  its_getProperty,  its_setProperty,
    its_enumerate,    (JSResolveOp)its_resolve,
    its_convert,      its_finalize,
    JSCLASS_NO_OPTIONAL_MEMBERS
};

JSErrorFormatString jsShell_ErrorFormatString[JSErr_Limit] = {
#define MSG_DEF(name, number, count, exception, format) \
    { format, count, JSEXN_ERR } ,
#include "jsshell.msg"
#undef MSG_DEF
};

static const JSErrorFormatString *
my_GetErrorMessage(void *userRef, const char *locale, const uintN errorNumber)
{
    if ((errorNumber > 0) && (errorNumber < JSShellErr_Limit))
        return &jsShell_ErrorFormatString[errorNumber];
    return NULL;
}

static void
my_ErrorReporter(JSContext *cx, const char *message, JSErrorReport *report)
{
    int i, j, k, n;
    char *prefix, *tmp;
    const char *ctmp;

    if (!report) {
        fprintf(gErrFile, "%s\n", message);
        return;
    }

    /* Conditionally ignore reported warnings. */
    if (JSREPORT_IS_WARNING(report->flags) && !reportWarnings)
        return;

    prefix = NULL;
    if (report->filename)
        prefix = JS_smprintf("%s:", report->filename);
    if (report->lineno) {
        tmp = prefix;
        prefix = JS_smprintf("%s%u: ", tmp ? tmp : "", report->lineno);
        JS_free(cx, tmp);
    }
    if (JSREPORT_IS_WARNING(report->flags)) {
        tmp = prefix;
        prefix = JS_smprintf("%s%swarning: ",
                             tmp ? tmp : "",
                             JSREPORT_IS_STRICT(report->flags) ? "strict " : "");
        JS_free(cx, tmp);
    }

    /* embedded newlines -- argh! */
    while ((ctmp = strchr(message, '\n')) != 0) {
        ctmp++;
        if (prefix)
            fputs(prefix, gErrFile);
        fwrite(message, 1, ctmp - message, gErrFile);
        message = ctmp;
    }

    /* If there were no filename or lineno, the prefix might be empty */
    if (prefix)
        fputs(prefix, gErrFile);
    fputs(message, gErrFile);

    if (!report->linebuf) {
        fputc('\n', gErrFile);
        goto out;
    }

    /* report->linebuf usually ends with a newline. */
    n = strlen(report->linebuf);
    fprintf(gErrFile, ":\n%s%s%s%s",
            prefix,
            report->linebuf,
            (n > 0 && report->linebuf[n-1] == '\n') ? "" : "\n",
            prefix);
    n = PTRDIFF(report->tokenptr, report->linebuf, char);
    for (i = j = 0; i < n; i++) {
        if (report->linebuf[i] == '\t') {
            for (k = (j + 8) & ~7; j < k; j++) {
                fputc('.', gErrFile);
            }
            continue;
        }
        fputc('.', gErrFile);
        j++;
    }
    fputs("^\n", gErrFile);
 out:
    if (!JSREPORT_IS_WARNING(report->flags)) {
        if (report->errorNumber == JSMSG_OUT_OF_MEMORY) {
            gExitCode = EXITCODE_OUT_OF_MEMORY;
        } else {
            gExitCode = EXITCODE_RUNTIME_ERROR;
        }
    }
    JS_free(cx, prefix);
}

#if defined(SHELL_HACK) && defined(DEBUG) && defined(XP_UNIX)
static JSBool
Exec(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval)
{
    JSFunction *fun;
    const char *name, **nargv;
    uintN i, nargc;
    JSString *str;
    pid_t pid;
    int status;

    fun = JS_ValueToFunction(cx, argv[-2]);
    if (!fun)
        return JS_FALSE;
    if (!fun->atom)
        return JS_TRUE;
    name = JS_GetStringBytes(ATOM_TO_STRING(fun->atom));
    nargc = 1 + argc;
    nargv = JS_malloc(cx, (nargc + 1) * sizeof(char *));
    if (!nargv)
        return JS_FALSE;
    nargv[0] = name;
    for (i = 1; i < nargc; i++) {
        str = JS_ValueToString(cx, argv[i-1]);
        if (!str) {
            JS_free(cx, nargv);
            return JS_FALSE;
        }
        nargv[i] = JS_GetStringBytes(str);
    }
    nargv[nargc] = 0;
    pid = fork();
    switch (pid) {
      case -1:
        perror("js");
        break;
      case 0:
        (void) execvp(name, (char **)nargv);
        perror("js");
        exit(127);
      default:
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
            continue;
        break;
    }
    JS_free(cx, nargv);
    return JS_TRUE;
}
#endif

static JSBool
global_enumerate(JSContext *cx, JSObject *obj)
{
#ifdef LAZY_STANDARD_CLASSES
    return JS_EnumerateStandardClasses(cx, obj);
#else
    return JS_TRUE;
#endif
}

static JSBool
global_resolve(JSContext *cx, JSObject *obj, jsval id, uintN flags,
               JSObject **objp)
{
#ifdef LAZY_STANDARD_CLASSES
    JSBool resolved;

    if (!JS_ResolveStandardClass(cx, obj, id, &resolved))
        return JS_FALSE;
    if (resolved) {
        *objp = obj;
        return JS_TRUE;
    }
#endif

#if defined(SHELL_HACK) && defined(DEBUG) && defined(XP_UNIX)
    if ((flags & (JSRESOLVE_QUALIFIED | JSRESOLVE_ASSIGNING)) == 0) {
        /*
         * Do this expensive hack only for unoptimized Unix builds, which are
         * not used for benchmarking.
         */
        char *path, *comp, *full;
        const char *name;
        JSBool ok, found;
        JSFunction *fun;

        if (!JSVAL_IS_STRING(id))
            return JS_TRUE;
        path = getenv("PATH");
        if (!path)
            return JS_TRUE;
        path = JS_strdup(cx, path);
        if (!path)
            return JS_FALSE;
        name = JS_GetStringBytes(JSVAL_TO_STRING(id));
        ok = JS_TRUE;
        for (comp = strtok(path, ":"); comp; comp = strtok(NULL, ":")) {
            if (*comp != '\0') {
                full = JS_smprintf("%s/%s", comp, name);
                if (!full) {
                    JS_ReportOutOfMemory(cx);
                    ok = JS_FALSE;
                    break;
                }
            } else {
                full = (char *)name;
            }
            found = (access(full, X_OK) == 0);
            if (*comp != '\0')
                free(full);
            if (found) {
                fun = JS_DefineFunction(cx, obj, name, Exec, 0,
                                        JSPROP_ENUMERATE);
                ok = (fun != NULL);
                if (ok)
                    *objp = obj;
                break;
            }
        }
        JS_free(cx, path);
        return ok;
    }
#else
    return JS_TRUE;
#endif
}

JSClass global_class = {
    "global", JSCLASS_NEW_RESOLVE | JSCLASS_GLOBAL_FLAGS,
    JS_PropertyStub,  JS_PropertyStub,
    JS_PropertyStub,  JS_PropertyStub,
    global_enumerate, (JSResolveOp) global_resolve,
    JS_ConvertStub,   JS_FinalizeStub,
    JSCLASS_NO_OPTIONAL_MEMBERS
};

static JSBool
env_setProperty(JSContext *cx, JSObject *obj, jsval id, jsval *vp)
{
/* XXX porting may be easy, but these don't seem to supply setenv by default */
#if !defined XP_BEOS && !defined XP_OS2 && !defined SOLARIS
    JSString *idstr, *valstr;
    const char *name, *value;
    int rv;

    idstr = JS_ValueToString(cx, id);
    valstr = JS_ValueToString(cx, *vp);
    if (!idstr || !valstr)
        return JS_FALSE;
    name = JS_GetStringBytes(idstr);
    value = JS_GetStringBytes(valstr);
#if defined XP_WIN || defined HPUX || defined OSF1 || defined IRIX
    {
        char *waste = JS_smprintf("%s=%s", name, value);
        if (!waste) {
            JS_ReportOutOfMemory(cx);
            return JS_FALSE;
        }
        rv = putenv(waste);
#ifdef XP_WIN
        /*
         * HPUX9 at least still has the bad old non-copying putenv.
         *
         * Per mail from <s.shanmuganathan@digital.com>, OSF1 also has a putenv
         * that will crash if you pass it an auto char array (so it must place
         * its argument directly in the char *environ[] array).
         */
        free(waste);
#endif
    }
#else
    rv = setenv(name, value, 1);
#endif
    if (rv < 0) {
        JS_ReportError(cx, "can't set envariable %s to %s", name, value);
        return JS_FALSE;
    }
    *vp = STRING_TO_JSVAL(valstr);
#endif /* !defined XP_BEOS && !defined XP_OS2 && !defined SOLARIS */
    return JS_TRUE;
}

static JSBool
env_enumerate(JSContext *cx, JSObject *obj)
{
    static JSBool reflected;
    char **evp, *name, *value;
    JSString *valstr;
    JSBool ok;

    if (reflected)
        return JS_TRUE;

    for (evp = (char **)JS_GetPrivate(cx, obj); (name = *evp) != NULL; evp++) {
        value = strchr(name, '=');
        if (!value)
            continue;
        *value++ = '\0';
        valstr = JS_NewStringCopyZ(cx, value);
        if (!valstr) {
            ok = JS_FALSE;
        } else {
            ok = JS_DefineProperty(cx, obj, name, STRING_TO_JSVAL(valstr),
                                   NULL, NULL, JSPROP_ENUMERATE);
        }
        value[-1] = '=';
        if (!ok)
            return JS_FALSE;
    }

    reflected = JS_TRUE;
    return JS_TRUE;
}

static JSBool
env_resolve(JSContext *cx, JSObject *obj, jsval id, uintN flags,
            JSObject **objp)
{
    JSString *idstr, *valstr;
    const char *name, *value;

    if (flags & JSRESOLVE_ASSIGNING)
        return JS_TRUE;

    idstr = JS_ValueToString(cx, id);
    if (!idstr)
        return JS_FALSE;
    name = JS_GetStringBytes(idstr);
    value = getenv(name);
    if (value) {
        valstr = JS_NewStringCopyZ(cx, value);
        if (!valstr)
            return JS_FALSE;
        if (!JS_DefineProperty(cx, obj, name, STRING_TO_JSVAL(valstr),
                               NULL, NULL, JSPROP_ENUMERATE)) {
            return JS_FALSE;
        }
        *objp = obj;
    }
    return JS_TRUE;
}

static JSClass env_class = {
    "environment", JSCLASS_HAS_PRIVATE | JSCLASS_NEW_RESOLVE,
    JS_PropertyStub,  JS_PropertyStub,
    JS_PropertyStub,  env_setProperty,
    env_enumerate, (JSResolveOp) env_resolve,
    JS_ConvertStub,   JS_FinalizeStub,
    JSCLASS_NO_OPTIONAL_MEMBERS
};

#ifdef NARCISSUS

static JSBool
defineProperty(JSContext *cx, JSObject *obj, uintN argc, jsval *argv,
               jsval *rval)
{
    JSString *str;
    jsval value;
    JSBool dontDelete, readOnly, dontEnum;
    const jschar *chars;
    size_t length;
    uintN attrs;

    dontDelete = readOnly = dontEnum = JS_FALSE;
    if (!JS_ConvertArguments(cx, argc, argv, "Sv/bbb",
                             &str, &value, &dontDelete, &readOnly, &dontEnum)) {
        return JS_FALSE;
    }
    chars = JS_GetStringChars(str);
    length = JS_GetStringLength(str);
    attrs = dontEnum ? 0 : JSPROP_ENUMERATE;
    if (dontDelete)
        attrs |= JSPROP_PERMANENT;
    if (readOnly)
        attrs |= JSPROP_READONLY;
    return JS_DefineUCProperty(cx, obj, chars, length, value, NULL, NULL,
                               attrs);
}

static JSBool
Evaluate(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval)
{
    /* function evaluate(source, filename, lineno) { ... } */
    JSString *source;
    const char *filename = "";
    jsuint lineno = 0;
    uint32 oldopts;
    JSBool ok;

    if (argc == 0) {
        *rval = JSVAL_VOID;
        return JS_TRUE;
    }

    if (!JS_ConvertArguments(cx, argc, argv, "S/su",
                             &source, &filename, &lineno)) {
        return JS_FALSE;
    }

    oldopts = JS_GetOptions(cx);
    JS_SetOptions(cx, oldopts | JSOPTION_COMPILE_N_GO);
    ok = JS_EvaluateUCScript(cx, obj, JS_GetStringChars(source),
                             JS_GetStringLength(source), filename,
                             lineno, rval);
    JS_SetOptions(cx, oldopts);

    return ok;
}

#include <fcntl.h>
#include <sys/stat.h>

/*
 * Returns a JS_malloc'd string (that the caller needs to JS_free)
 * containing the directory (non-leaf) part of |from| prepended to |leaf|.
 * If |from| is empty or a leaf, MakeAbsolutePathname returns a copy of leaf.
 * Returns NULL to indicate an error.
 */
static char *
MakeAbsolutePathname(JSContext *cx, const char *from, const char *leaf)
{
    size_t dirlen;
    char *dir;
    const char *slash = NULL, *cp;

    cp = from;
    while (*cp) {
        if (*cp == '/'
#ifdef XP_WIN
            || *cp == '\\'
#endif
           ) {
            slash = cp;
        }

        ++cp;
    }

    if (!slash) {
        /* We were given a leaf or |from| was empty. */
        return JS_strdup(cx, leaf);
    }

    /* Else, we were given a real pathname, return that + the leaf. */
    dirlen = slash - from + 1;
    dir = JS_malloc(cx, dirlen + strlen(leaf) + 1);
    if (!dir)
        return NULL;

    strncpy(dir, from, dirlen);
    strcpy(dir + dirlen, leaf); /* Note: we can't use strcat here. */

    return dir;
}

static JSBool
snarf(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval)
{
    JSString *str;
    const char *filename;
    char *pathname;
    JSStackFrame *fp;
    JSBool ok;
    off_t cc, len;
    char *buf;
    FILE *file;

    str = JS_ValueToString(cx, argv[0]);
    if (!str)
        return JS_FALSE;
    filename = JS_GetStringBytes(str);

    /* Get the currently executing script's name. */
    fp = JS_GetScriptedCaller(cx, NULL);
    JS_ASSERT(fp && fp->script->filename);
    pathname = MakeAbsolutePathname(cx, fp->script->filename, filename);
    if (!pathname)
        return JS_FALSE;

    ok = JS_FALSE;
    len = 0;
    buf = NULL;
    file = fopen(pathname, "rb");
    if (!file) {
        JS_ReportError(cx, "can't open %s: %s", pathname, strerror(errno));
    } else {
        if (fseek(file, 0, SEEK_END) == EOF) {
            JS_ReportError(cx, "can't seek end of %s", pathname);
        } else {
            len = ftell(file);
            if (fseek(file, 0, SEEK_SET) == EOF) {
                JS_ReportError(cx, "can't seek start of %s", pathname);
            } else {
                buf = JS_malloc(cx, len + 1);
                if (buf) {
                    cc = fread(buf, 1, len, file);
                    if (cc != len) {
                        JS_free(cx, buf);
                        JS_ReportError(cx, "can't read %s: %s", pathname,
                                       (cc < 0) ? strerror(errno)
                                                : "short read");
                    } else {
                        len = (size_t)cc;
                        ok = JS_TRUE;
                    }
                }
            }
        }
        fclose(file);
    }
    JS_free(cx, pathname);
    if (!ok) {
        JS_free(cx, buf);
        return ok;
    }

    buf[len] = '\0';
    str = JS_NewString(cx, buf, len);
    if (!str) {
        JS_free(cx, buf);
        return JS_FALSE;
    }
    *rval = STRING_TO_JSVAL(str);
    return JS_TRUE;
}

#endif /* NARCISSUS */

int
main(int argc, char **argv, char **envp)
{
    int stackDummy;
    JSRuntime *rt;
    JSContext *cx;
    JSObject *glob, *it, *envobj;
    int result;
#ifdef LIVECONNECT
    JavaVM *java_vm = NULL;
#endif
#ifdef JSDEBUGGER_JAVA_UI
    JNIEnv *java_env;
#endif

    gStackBase = (jsuword)&stackDummy;

    setlocale(LC_ALL, "");

#ifdef XP_OS2
   /* these streams are normally line buffered on OS/2 and need a \n, *
    * so we need to unbuffer then to get a reasonable prompt          */
    setbuf(stdout,0);
    setbuf(stderr,0);
#endif

    gErrFile = stderr;
    gOutFile = stdout;

    argc--;
    argv++;

    rt = JS_NewRuntime(64L * 1024L * 1024L);
    if (!rt)
        return 1;

    cx = JS_NewContext(rt, gStackChunkSize);
    if (!cx)
        return 1;
    JS_SetErrorReporter(cx, my_ErrorReporter);

#ifdef JS_THREADSAFE
    JS_BeginRequest(cx);
#endif

    glob = JS_NewObject(cx, &global_class, NULL, NULL);
    if (!glob)
        return 1;
#ifdef LAZY_STANDARD_CLASSES
    JS_SetGlobalObject(cx, glob);
#else
    if (!JS_InitStandardClasses(cx, glob))
        return 1;
#endif
    if (!JS_DefineFunctions(cx, glob, shell_functions))
        return 1;

    it = JS_DefineObject(cx, glob, "it", &its_class, NULL, 0);
    if (!it)
        return 1;
    if (!JS_DefineProperties(cx, it, its_props))
        return 1;
    if (!JS_DefineFunctions(cx, it, its_methods))
        return 1;

#ifdef PERLCONNECT
    if (!JS_InitPerlClass(cx, glob))
        return 1;
#endif

#ifdef JSDEBUGGER
    /*
    * XXX A command line option to enable debugging (or not) would be good
    */
    _jsdc = JSD_DebuggerOnForUser(rt, NULL, NULL);
    if (!_jsdc)
        return 1;
    JSD_JSContextInUse(_jsdc, cx);
#ifdef JSD_LOWLEVEL_SOURCE
    JS_SetSourceHandler(rt, SendSourceToJSDebugger, _jsdc);
#endif /* JSD_LOWLEVEL_SOURCE */
#ifdef JSDEBUGGER_JAVA_UI
    _jsdjc = JSDJ_CreateContext();
    if (! _jsdjc)
        return 1;
    JSDJ_SetJSDContext(_jsdjc, _jsdc);
    java_env = JSDJ_CreateJavaVMAndStartDebugger(_jsdjc);
#ifdef LIVECONNECT
    if (java_env)
        (*java_env)->GetJavaVM(java_env, &java_vm);
#endif
    /*
    * XXX This would be the place to wait for the debugger to start.
    * Waiting would be nice in general, but especially when a js file
    * is passed on the cmd line.
    */
#endif /* JSDEBUGGER_JAVA_UI */
#ifdef JSDEBUGGER_C_UI
    JSDB_InitDebugger(rt, _jsdc, 0);
#endif /* JSDEBUGGER_C_UI */
#endif /* JSDEBUGGER */

#ifdef LIVECONNECT
    if (!JSJ_SimpleInit(cx, glob, java_vm, getenv("CLASSPATH")))
        return 1;
#endif

    envobj = JS_DefineObject(cx, glob, "environment", &env_class, NULL, 0);
    if (!envobj || !JS_SetPrivate(cx, envobj, envp))
        return 1;

#ifdef NARCISSUS
    {
        jsval v;
        static const char Object_prototype[] = "Object.prototype";

        if (!JS_DefineFunction(cx, glob, "snarf", snarf, 1, 0))
            return 1;
        if (!JS_DefineFunction(cx, glob, "evaluate", Evaluate, 3, 0))
            return 1;

        if (!JS_EvaluateScript(cx, glob,
                               Object_prototype, sizeof Object_prototype - 1,
                               NULL, 0, &v)) {
            return 1;
        }
        if (!JS_DefineFunction(cx, JSVAL_TO_OBJECT(v), "__defineProperty__",
                               defineProperty, 5, 0)) {
            return 1;
        }
    }
#endif

    result = ProcessArgs(cx, glob, argv, argc);

#ifdef JSDEBUGGER
    if (_jsdc)
        JSD_DebuggerOff(_jsdc);
#endif  /* JSDEBUGGER */

#ifdef JS_THREADSAFE
    JS_EndRequest(cx);
#endif

    JS_DestroyContext(cx);
    JS_DestroyRuntime(rt);
    JS_ShutDown();
    return result;
}
