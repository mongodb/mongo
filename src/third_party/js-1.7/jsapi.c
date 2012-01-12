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
 * JavaScript API.
 */
#include "jsstddef.h"
#include <ctype.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include "jstypes.h"
#include "jsarena.h" /* Added by JSIFY */
#include "jsutil.h" /* Added by JSIFY */
#include "jsclist.h"
#include "jsdhash.h"
#include "jsprf.h"
#include "jsapi.h"
#include "jsarray.h"
#include "jsatom.h"
#include "jsbool.h"
#include "jscntxt.h"
#include "jsconfig.h"
#include "jsdate.h"
#include "jsdtoa.h"
#include "jsemit.h"
#include "jsexn.h"
#include "jsfun.h"
#include "jsgc.h"
#include "jsinterp.h"
#include "jslock.h"
#include "jsmath.h"
#include "jsnum.h"
#include "jsobj.h"
#include "jsopcode.h"
#include "jsparse.h"
#include "jsregexp.h"
#include "jsscan.h"
#include "jsscope.h"
#include "jsscript.h"
#include "jsstr.h"
#include "prmjtime.h"

#if JS_HAS_FILE_OBJECT
#include "jsfile.h"
#endif

#if JS_HAS_XML_SUPPORT
#include "jsxml.h"
#endif

#if JS_HAS_GENERATORS
#include "jsiter.h"
#endif

#ifdef HAVE_VA_LIST_AS_ARRAY
#define JS_ADDRESSOF_VA_LIST(ap) ((va_list *)(ap))
#else
#define JS_ADDRESSOF_VA_LIST(ap) (&(ap))
#endif

#if defined(JS_PARANOID_REQUEST) && defined(JS_THREADSAFE)
#define CHECK_REQUEST(cx)       JS_ASSERT(cx->requestDepth)
#else
#define CHECK_REQUEST(cx)       ((void)0)
#endif

JS_PUBLIC_API(int64)
JS_Now()
{
    return PRMJ_Now();
}

JS_PUBLIC_API(jsval)
JS_GetNaNValue(JSContext *cx)
{
    return DOUBLE_TO_JSVAL(cx->runtime->jsNaN);
}

JS_PUBLIC_API(jsval)
JS_GetNegativeInfinityValue(JSContext *cx)
{
    return DOUBLE_TO_JSVAL(cx->runtime->jsNegativeInfinity);
}

JS_PUBLIC_API(jsval)
JS_GetPositiveInfinityValue(JSContext *cx)
{
    return DOUBLE_TO_JSVAL(cx->runtime->jsPositiveInfinity);
}

JS_PUBLIC_API(jsval)
JS_GetEmptyStringValue(JSContext *cx)
{
    return STRING_TO_JSVAL(cx->runtime->emptyString);
}

static JSBool
TryArgumentFormatter(JSContext *cx, const char **formatp, JSBool fromJS,
                     jsval **vpp, va_list *app)
{
    const char *format;
    JSArgumentFormatMap *map;

    format = *formatp;
    for (map = cx->argumentFormatMap; map; map = map->next) {
        if (!strncmp(format, map->format, map->length)) {
            *formatp = format + map->length;
            return map->formatter(cx, format, fromJS, vpp, app);
        }
    }
    JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_BAD_CHAR, format);
    return JS_FALSE;
}

JS_PUBLIC_API(JSBool)
JS_ConvertArguments(JSContext *cx, uintN argc, jsval *argv, const char *format,
                    ...)
{
    va_list ap;
    JSBool ok;

    va_start(ap, format);
    ok = JS_ConvertArgumentsVA(cx, argc, argv, format, ap);
    va_end(ap);
    return ok;
}

JS_PUBLIC_API(JSBool)
JS_ConvertArgumentsVA(JSContext *cx, uintN argc, jsval *argv,
                      const char *format, va_list ap)
{
    jsval *sp;
    JSBool required;
    char c;
    JSFunction *fun;
    jsdouble d;
    JSString *str;
    JSObject *obj;

    CHECK_REQUEST(cx);
    sp = argv;
    required = JS_TRUE;
    while ((c = *format++) != '\0') {
        if (isspace(c))
            continue;
        if (c == '/') {
            required = JS_FALSE;
            continue;
        }
        if (sp == argv + argc) {
            if (required) {
                fun = js_ValueToFunction(cx, &argv[-2], 0);
                if (fun) {
                    char numBuf[12];
                    JS_snprintf(numBuf, sizeof numBuf, "%u", argc);
                    JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL,
                                         JSMSG_MORE_ARGS_NEEDED,
                                         JS_GetFunctionName(fun), numBuf,
                                         (argc == 1) ? "" : "s");
                }
                return JS_FALSE;
            }
            break;
        }
        switch (c) {
          case 'b':
            if (!js_ValueToBoolean(cx, *sp, va_arg(ap, JSBool *)))
                return JS_FALSE;
            break;
          case 'c':
            if (!js_ValueToUint16(cx, *sp, va_arg(ap, uint16 *)))
                return JS_FALSE;
            break;
          case 'i':
            if (!js_ValueToECMAInt32(cx, *sp, va_arg(ap, int32 *)))
                return JS_FALSE;
            break;
          case 'u':
            if (!js_ValueToECMAUint32(cx, *sp, va_arg(ap, uint32 *)))
                return JS_FALSE;
            break;
          case 'j':
            if (!js_ValueToInt32(cx, *sp, va_arg(ap, int32 *)))
                return JS_FALSE;
            break;
          case 'd':
            if (!js_ValueToNumber(cx, *sp, va_arg(ap, jsdouble *)))
                return JS_FALSE;
            break;
          case 'I':
            if (!js_ValueToNumber(cx, *sp, &d))
                return JS_FALSE;
            *va_arg(ap, jsdouble *) = js_DoubleToInteger(d);
            break;
          case 's':
          case 'S':
          case 'W':
            str = js_ValueToString(cx, *sp);
            if (!str)
                return JS_FALSE;
            *sp = STRING_TO_JSVAL(str);
            if (c == 's')
                *va_arg(ap, char **) = JS_GetStringBytes(str);
            else if (c == 'W')
                *va_arg(ap, jschar **) = JS_GetStringChars(str);
            else
                *va_arg(ap, JSString **) = str;
            break;
          case 'o':
            if (!js_ValueToObject(cx, *sp, &obj))
                return JS_FALSE;
            *sp = OBJECT_TO_JSVAL(obj);
            *va_arg(ap, JSObject **) = obj;
            break;
          case 'f':
            obj = js_ValueToFunctionObject(cx, sp, 0);
            if (!obj)
                return JS_FALSE;
            *va_arg(ap, JSFunction **) = (JSFunction *) JS_GetPrivate(cx, obj);
            break;
          case 'v':
            *va_arg(ap, jsval *) = *sp;
            break;
          case '*':
            break;
          default:
            format--;
            if (!TryArgumentFormatter(cx, &format, JS_TRUE, &sp,
                                      JS_ADDRESSOF_VA_LIST(ap))) {
                return JS_FALSE;
            }
            /* NB: the formatter already updated sp, so we continue here. */
            continue;
        }
        sp++;
    }
    return JS_TRUE;
}

JS_PUBLIC_API(jsval *)
JS_PushArguments(JSContext *cx, void **markp, const char *format, ...)
{
    va_list ap;
    jsval *argv;

    va_start(ap, format);
    argv = JS_PushArgumentsVA(cx, markp, format, ap);
    va_end(ap);
    return argv;
}

JS_PUBLIC_API(jsval *)
JS_PushArgumentsVA(JSContext *cx, void **markp, const char *format, va_list ap)
{
    uintN argc;
    jsval *argv, *sp;
    char c;
    const char *cp;
    JSString *str;
    JSFunction *fun;
    JSStackHeader *sh;

    CHECK_REQUEST(cx);
    *markp = NULL;
    argc = 0;
    for (cp = format; (c = *cp) != '\0'; cp++) {
        /*
         * Count non-space non-star characters as individual jsval arguments.
         * This may over-allocate stack, but we'll fix below.
         */
        if (isspace(c) || c == '*')
            continue;
        argc++;
    }
    sp = js_AllocStack(cx, argc, markp);
    if (!sp)
        return NULL;
    argv = sp;
    while ((c = *format++) != '\0') {
        if (isspace(c) || c == '*')
            continue;
        switch (c) {
          case 'b':
            *sp = BOOLEAN_TO_JSVAL((JSBool) va_arg(ap, int));
            break;
          case 'c':
            *sp = INT_TO_JSVAL((uint16) va_arg(ap, unsigned int));
            break;
          case 'i':
          case 'j':
            if (!js_NewNumberValue(cx, (jsdouble) va_arg(ap, int32), sp))
                goto bad;
            break;
          case 'u':
            if (!js_NewNumberValue(cx, (jsdouble) va_arg(ap, uint32), sp))
                goto bad;
            break;
          case 'd':
          case 'I':
            if (!js_NewDoubleValue(cx, va_arg(ap, jsdouble), sp))
                goto bad;
            break;
          case 's':
            str = JS_NewStringCopyZ(cx, va_arg(ap, char *));
            if (!str)
                goto bad;
            *sp = STRING_TO_JSVAL(str);
            break;
          case 'W':
            str = JS_NewUCStringCopyZ(cx, va_arg(ap, jschar *));
            if (!str)
                goto bad;
            *sp = STRING_TO_JSVAL(str);
            break;
          case 'S':
            str = va_arg(ap, JSString *);
            *sp = STRING_TO_JSVAL(str);
            break;
          case 'o':
            *sp = OBJECT_TO_JSVAL(va_arg(ap, JSObject *));
            break;
          case 'f':
            fun = va_arg(ap, JSFunction *);
            *sp = fun ? OBJECT_TO_JSVAL(fun->object) : JSVAL_NULL;
            break;
          case 'v':
            *sp = va_arg(ap, jsval);
            break;
          default:
            format--;
            if (!TryArgumentFormatter(cx, &format, JS_FALSE, &sp,
                                      JS_ADDRESSOF_VA_LIST(ap))) {
                goto bad;
            }
            /* NB: the formatter already updated sp, so we continue here. */
            continue;
        }
        sp++;
    }

    /*
     * We may have overallocated stack due to a multi-character format code
     * handled by a JSArgumentFormatter.  Give back that stack space!
     */
    JS_ASSERT(sp <= argv + argc);
    if (sp < argv + argc) {
        /* Return slots not pushed to the current stack arena. */
        cx->stackPool.current->avail = (jsuword)sp;

        /* Reduce the count of slots the GC will scan in this stack segment. */
        sh = cx->stackHeaders;
        JS_ASSERT(JS_STACK_SEGMENT(sh) + sh->nslots == argv + argc);
        sh->nslots -= argc - (sp - argv);
    }
    return argv;

bad:
    js_FreeStack(cx, *markp);
    return NULL;
}

JS_PUBLIC_API(void)
JS_PopArguments(JSContext *cx, void *mark)
{
    CHECK_REQUEST(cx);
    js_FreeStack(cx, mark);
}

JS_PUBLIC_API(JSBool)
JS_AddArgumentFormatter(JSContext *cx, const char *format,
                        JSArgumentFormatter formatter)
{
    size_t length;
    JSArgumentFormatMap **mpp, *map;

    length = strlen(format);
    mpp = &cx->argumentFormatMap;
    while ((map = *mpp) != NULL) {
        /* Insert before any shorter string to match before prefixes. */
        if (map->length < length)
            break;
        if (map->length == length && !strcmp(map->format, format))
            goto out;
        mpp = &map->next;
    }
    map = (JSArgumentFormatMap *) JS_malloc(cx, sizeof *map);
    if (!map)
        return JS_FALSE;
    map->format = format;
    map->length = length;
    map->next = *mpp;
    *mpp = map;
out:
    map->formatter = formatter;
    return JS_TRUE;
}

JS_PUBLIC_API(void)
JS_RemoveArgumentFormatter(JSContext *cx, const char *format)
{
    size_t length;
    JSArgumentFormatMap **mpp, *map;

    length = strlen(format);
    mpp = &cx->argumentFormatMap;
    while ((map = *mpp) != NULL) {
        if (map->length == length && !strcmp(map->format, format)) {
            *mpp = map->next;
            JS_free(cx, map);
            return;
        }
        mpp = &map->next;
    }
}

JS_PUBLIC_API(JSBool)
JS_ConvertValue(JSContext *cx, jsval v, JSType type, jsval *vp)
{
    JSBool ok, b;
    JSObject *obj;
    JSString *str;
    jsdouble d, *dp;

    CHECK_REQUEST(cx);
    switch (type) {
      case JSTYPE_VOID:
        *vp = JSVAL_VOID;
        ok = JS_TRUE;
        break;
      case JSTYPE_OBJECT:
        ok = js_ValueToObject(cx, v, &obj);
        if (ok)
            *vp = OBJECT_TO_JSVAL(obj);
        break;
      case JSTYPE_FUNCTION:
        *vp = v;
        obj = js_ValueToFunctionObject(cx, vp, JSV2F_SEARCH_STACK);
        ok = (obj != NULL);
        break;
      case JSTYPE_STRING:
        str = js_ValueToString(cx, v);
        ok = (str != NULL);
        if (ok)
            *vp = STRING_TO_JSVAL(str);
        break;
      case JSTYPE_NUMBER:
        ok = js_ValueToNumber(cx, v, &d);
        if (ok) {
            dp = js_NewDouble(cx, d, 0);
            ok = (dp != NULL);
            if (ok)
                *vp = DOUBLE_TO_JSVAL(dp);
        }
        break;
      case JSTYPE_BOOLEAN:
        ok = js_ValueToBoolean(cx, v, &b);
        if (ok)
            *vp = BOOLEAN_TO_JSVAL(b);
        break;
      default: {
        char numBuf[12];
        JS_snprintf(numBuf, sizeof numBuf, "%d", (int)type);
        JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_BAD_TYPE,
                             numBuf);
        ok = JS_FALSE;
        break;
      }
    }
    return ok;
}

JS_PUBLIC_API(JSBool)
JS_ValueToObject(JSContext *cx, jsval v, JSObject **objp)
{
    CHECK_REQUEST(cx);
    return js_ValueToObject(cx, v, objp);
}

JS_PUBLIC_API(JSFunction *)
JS_ValueToFunction(JSContext *cx, jsval v)
{
    CHECK_REQUEST(cx);
    return js_ValueToFunction(cx, &v, JSV2F_SEARCH_STACK);
}

JS_PUBLIC_API(JSFunction *)
JS_ValueToConstructor(JSContext *cx, jsval v)
{
    CHECK_REQUEST(cx);
    return js_ValueToFunction(cx, &v, JSV2F_SEARCH_STACK);
}

JS_PUBLIC_API(JSString *)
JS_ValueToString(JSContext *cx, jsval v)
{
    CHECK_REQUEST(cx);
    return js_ValueToString(cx, v);
}

JS_PUBLIC_API(JSBool)
JS_ValueToNumber(JSContext *cx, jsval v, jsdouble *dp)
{
    CHECK_REQUEST(cx);
    return js_ValueToNumber(cx, v, dp);
}

JS_PUBLIC_API(JSBool)
JS_ValueToECMAInt32(JSContext *cx, jsval v, int32 *ip)
{
    CHECK_REQUEST(cx);
    return js_ValueToECMAInt32(cx, v, ip);
}

JS_PUBLIC_API(JSBool)
JS_ValueToECMAUint32(JSContext *cx, jsval v, uint32 *ip)
{
    CHECK_REQUEST(cx);
    return js_ValueToECMAUint32(cx, v, ip);
}

JS_PUBLIC_API(JSBool)
JS_ValueToInt32(JSContext *cx, jsval v, int32 *ip)
{
    CHECK_REQUEST(cx);
    return js_ValueToInt32(cx, v, ip);
}

JS_PUBLIC_API(JSBool)
JS_ValueToUint16(JSContext *cx, jsval v, uint16 *ip)
{
    CHECK_REQUEST(cx);
    return js_ValueToUint16(cx, v, ip);
}

JS_PUBLIC_API(JSBool)
JS_ValueToBoolean(JSContext *cx, jsval v, JSBool *bp)
{
    CHECK_REQUEST(cx);
    return js_ValueToBoolean(cx, v, bp);
}

JS_PUBLIC_API(JSType)
JS_TypeOfValue(JSContext *cx, jsval v)
{
    JSType type;
    JSObject *obj;
    JSObjectOps *ops;
    JSClass *clasp;

    CHECK_REQUEST(cx);
    if (JSVAL_IS_OBJECT(v)) {
        type = JSTYPE_OBJECT;           /* XXXbe JSTYPE_NULL for JS2 */
        obj = JSVAL_TO_OBJECT(v);
        if (obj) {
            ops = obj->map->ops;
#if JS_HAS_XML_SUPPORT
            if (ops == &js_XMLObjectOps.base) {
                type = JSTYPE_XML;
            } else
#endif
            {
                /*
                 * ECMA 262, 11.4.3 says that any native object that implements
                 * [[Call]] should be of type "function". Note that RegExp and
                 * Script are both of type "function" for compatibility with
                 * older SpiderMonkeys.
                 */
                clasp = OBJ_GET_CLASS(cx, obj);
                if ((ops == &js_ObjectOps)
                    ? (clasp->call
                       ? (clasp == &js_RegExpClass || clasp == &js_ScriptClass)
                       : clasp == &js_FunctionClass)
                    : ops->call != NULL) {
                    type = JSTYPE_FUNCTION;
                } else {
#ifdef NARCISSUS
                    if (!OBJ_GET_PROPERTY(cx, obj,
                                          ATOM_TO_JSID(cx->runtime->atomState
                                                       .callAtom),
                                          &v)) {
                        JS_ClearPendingException(cx);
                    } else if (VALUE_IS_FUNCTION(cx, v)) {
                        type = JSTYPE_FUNCTION;
                    }
#endif
                }
            }
        }
    } else if (JSVAL_IS_NUMBER(v)) {
        type = JSTYPE_NUMBER;
    } else if (JSVAL_IS_STRING(v)) {
        type = JSTYPE_STRING;
    } else if (JSVAL_IS_BOOLEAN(v)) {
        type = JSTYPE_BOOLEAN;
    } else {
        type = JSTYPE_VOID;
    }
    return type;
}

JS_PUBLIC_API(const char *)
JS_GetTypeName(JSContext *cx, JSType type)
{
    if ((uintN)type >= (uintN)JSTYPE_LIMIT)
        return NULL;
    return js_type_strs[type];
}

/************************************************************************/

JS_PUBLIC_API(JSRuntime *)
JS_NewRuntime(uint32 maxbytes)
{
    JSRuntime *rt;

#ifdef DEBUG
    static JSBool didFirstChecks;

    if (!didFirstChecks) {
        /*
         * This code asserts that the numbers associated with the error names
         * in jsmsg.def are monotonically increasing.  It uses values for the
         * error names enumerated in jscntxt.c.  It's not a compile-time check
         * but it's better than nothing.
         */
        int errorNumber = 0;
#define MSG_DEF(name, number, count, exception, format)                       \
    JS_ASSERT(name == errorNumber++);
#include "js.msg"
#undef MSG_DEF

#define MSG_DEF(name, number, count, exception, format)                       \
    JS_BEGIN_MACRO                                                            \
        uintN numfmtspecs = 0;                                                \
        const char *fmt;                                                      \
        for (fmt = format; *fmt != '\0'; fmt++) {                             \
            if (*fmt == '{' && isdigit(fmt[1]))                               \
                ++numfmtspecs;                                                \
        }                                                                     \
        JS_ASSERT(count == numfmtspecs);                                      \
    JS_END_MACRO;
#include "js.msg"
#undef MSG_DEF

        didFirstChecks = JS_TRUE;
    }
#endif /* DEBUG */

    rt = (JSRuntime *) malloc(sizeof(JSRuntime));
    if (!rt)
        return NULL;

    /* Initialize infallibly first, so we can goto bad and JS_DestroyRuntime. */
    memset(rt, 0, sizeof(JSRuntime));
    JS_INIT_CLIST(&rt->contextList);
    JS_INIT_CLIST(&rt->trapList);
    JS_INIT_CLIST(&rt->watchPointList);

    if (!js_InitGC(rt, maxbytes))
        goto bad;
#ifdef JS_THREADSAFE
    if (PR_FAILURE == PR_NewThreadPrivateIndex(&rt->threadTPIndex,
                                               js_ThreadDestructorCB)) {
        goto bad;
    }
    rt->gcLock = JS_NEW_LOCK();
    if (!rt->gcLock)
        goto bad;
    rt->gcDone = JS_NEW_CONDVAR(rt->gcLock);
    if (!rt->gcDone)
        goto bad;
    rt->requestDone = JS_NEW_CONDVAR(rt->gcLock);
    if (!rt->requestDone)
        goto bad;
    /* this is asymmetric with JS_ShutDown: */
    if (!js_SetupLocks(8, 16))
        goto bad;
    rt->rtLock = JS_NEW_LOCK();
    if (!rt->rtLock)
        goto bad;
    rt->stateChange = JS_NEW_CONDVAR(rt->gcLock);
    if (!rt->stateChange)
        goto bad;
    rt->setSlotLock = JS_NEW_LOCK();
    if (!rt->setSlotLock)
        goto bad;
    rt->setSlotDone = JS_NEW_CONDVAR(rt->setSlotLock);
    if (!rt->setSlotDone)
        goto bad;
    rt->scopeSharingDone = JS_NEW_CONDVAR(rt->gcLock);
    if (!rt->scopeSharingDone)
        goto bad;
    rt->scopeSharingTodo = NO_SCOPE_SHARING_TODO;
#endif
    rt->propertyCache.empty = JS_TRUE;
    if (!js_InitPropertyTree(rt))
        goto bad;
    return rt;

bad:
    JS_DestroyRuntime(rt);
    return NULL;
}

JS_PUBLIC_API(void)
JS_DestroyRuntime(JSRuntime *rt)
{
#ifdef DEBUG
    /* Don't hurt everyone in leaky ol' Mozilla with a fatal JS_ASSERT! */
    if (!JS_CLIST_IS_EMPTY(&rt->contextList)) {
        JSContext *cx, *iter = NULL;
        uintN cxcount = 0;
        while ((cx = js_ContextIterator(rt, JS_TRUE, &iter)) != NULL)
            cxcount++;
        fprintf(stderr,
"JS API usage error: %u contexts left in runtime upon JS_DestroyRuntime.\n",
                cxcount);
    }
#endif

    js_FreeRuntimeScriptState(rt);
    js_FinishAtomState(&rt->atomState);
    js_FinishGC(rt);
#ifdef JS_THREADSAFE
    if (rt->gcLock)
        JS_DESTROY_LOCK(rt->gcLock);
    if (rt->gcDone)
        JS_DESTROY_CONDVAR(rt->gcDone);
    if (rt->requestDone)
        JS_DESTROY_CONDVAR(rt->requestDone);
    if (rt->rtLock)
        JS_DESTROY_LOCK(rt->rtLock);
    if (rt->stateChange)
        JS_DESTROY_CONDVAR(rt->stateChange);
    if (rt->setSlotLock)
        JS_DESTROY_LOCK(rt->setSlotLock);
    if (rt->setSlotDone)
        JS_DESTROY_CONDVAR(rt->setSlotDone);
    if (rt->scopeSharingDone)
        JS_DESTROY_CONDVAR(rt->scopeSharingDone);
#else
    GSN_CACHE_CLEAR(&rt->gsnCache);
#endif
    js_FinishPropertyTree(rt);
    free(rt);
}

JS_PUBLIC_API(void)
JS_ShutDown(void)
{
    js_FinishDtoa();
#ifdef JS_THREADSAFE
    js_CleanupLocks();
#endif
}

JS_PUBLIC_API(void *)
JS_GetRuntimePrivate(JSRuntime *rt)
{
    return rt->data;
}

JS_PUBLIC_API(void)
JS_SetRuntimePrivate(JSRuntime *rt, void *data)
{
    rt->data = data;
}

#ifdef JS_THREADSAFE

JS_PUBLIC_API(void)
JS_BeginRequest(JSContext *cx)
{
    JSRuntime *rt;

    JS_ASSERT(cx->thread->id == js_CurrentThreadId());
    if (!cx->requestDepth) {
        /* Wait until the GC is finished. */
        rt = cx->runtime;
        JS_LOCK_GC(rt);

        /* NB: we use cx->thread here, not js_GetCurrentThread(). */
        if (rt->gcThread != cx->thread) {
            while (rt->gcLevel > 0)
                JS_AWAIT_GC_DONE(rt);
        }

        /* Indicate that a request is running. */
        rt->requestCount++;
        cx->requestDepth = 1;
        JS_UNLOCK_GC(rt);
        return;
    }
    cx->requestDepth++;
}

JS_PUBLIC_API(void)
JS_EndRequest(JSContext *cx)
{
    JSRuntime *rt;
    JSScope *scope, **todop;
    uintN nshares;

    CHECK_REQUEST(cx);
    JS_ASSERT(cx->requestDepth > 0);
    if (cx->requestDepth == 1) {
        /* Lock before clearing to interlock with ClaimScope, in jslock.c. */
        rt = cx->runtime;
        JS_LOCK_GC(rt);
        cx->requestDepth = 0;

        /* See whether cx has any single-threaded scopes to start sharing. */
        todop = &rt->scopeSharingTodo;
        nshares = 0;
        while ((scope = *todop) != NO_SCOPE_SHARING_TODO) {
            if (scope->ownercx != cx) {
                todop = &scope->u.link;
                continue;
            }
            *todop = scope->u.link;
            scope->u.link = NULL;       /* null u.link for sanity ASAP */

            /*
             * If js_DropObjectMap returns null, we held the last ref to scope.
             * The waiting thread(s) must have been killed, after which the GC
             * collected the object that held this scope.  Unlikely, because it
             * requires that the GC ran (e.g., from a branch callback) during
             * this request, but possible.
             */
            if (js_DropObjectMap(cx, &scope->map, NULL)) {
                js_InitLock(&scope->lock);
                scope->u.count = 0;                 /* NULL may not pun as 0 */
                js_FinishSharingScope(rt, scope);   /* set ownercx = NULL */
                nshares++;
            }
        }
        if (nshares)
            JS_NOTIFY_ALL_CONDVAR(rt->scopeSharingDone);

        /* Give the GC a chance to run if this was the last request running. */
        JS_ASSERT(rt->requestCount > 0);
        rt->requestCount--;
        if (rt->requestCount == 0)
            JS_NOTIFY_REQUEST_DONE(rt);

        JS_UNLOCK_GC(rt);
        return;
    }

    cx->requestDepth--;
}

/* Yield to pending GC operations, regardless of request depth */
JS_PUBLIC_API(void)
JS_YieldRequest(JSContext *cx)
{
    JSRuntime *rt;

    JS_ASSERT(cx->thread);
    CHECK_REQUEST(cx);

    rt = cx->runtime;
    JS_LOCK_GC(rt);
    JS_ASSERT(rt->requestCount > 0);
    rt->requestCount--;
    if (rt->requestCount == 0)
        JS_NOTIFY_REQUEST_DONE(rt);
    JS_UNLOCK_GC(rt);
    /* XXXbe give the GC or another request calling it a chance to run here?
             Assumes FIFO scheduling */
    JS_LOCK_GC(rt);
    if (rt->gcThread != cx->thread) {
        while (rt->gcLevel > 0)
            JS_AWAIT_GC_DONE(rt);
    }
    rt->requestCount++;
    JS_UNLOCK_GC(rt);
}

JS_PUBLIC_API(jsrefcount)
JS_SuspendRequest(JSContext *cx)
{
    jsrefcount saveDepth = cx->requestDepth;

    while (cx->requestDepth)
        JS_EndRequest(cx);
    return saveDepth;
}

JS_PUBLIC_API(void)
JS_ResumeRequest(JSContext *cx, jsrefcount saveDepth)
{
    JS_ASSERT(!cx->requestDepth);
    while (--saveDepth >= 0)
        JS_BeginRequest(cx);
}

#endif /* JS_THREADSAFE */

JS_PUBLIC_API(void)
JS_Lock(JSRuntime *rt)
{
    JS_LOCK_RUNTIME(rt);
}

JS_PUBLIC_API(void)
JS_Unlock(JSRuntime *rt)
{
    JS_UNLOCK_RUNTIME(rt);
}

JS_PUBLIC_API(JSContextCallback)
JS_SetContextCallback(JSRuntime *rt, JSContextCallback cxCallback)
{
    JSContextCallback old;

    old = rt->cxCallback;
    rt->cxCallback = cxCallback;
    return old;
}

JS_PUBLIC_API(JSContext *)
JS_NewContext(JSRuntime *rt, size_t stackChunkSize)
{
    return js_NewContext(rt, stackChunkSize);
}

JS_PUBLIC_API(void)
JS_DestroyContext(JSContext *cx)
{
    js_DestroyContext(cx, JSDCM_FORCE_GC);
}

JS_PUBLIC_API(void)
JS_DestroyContextNoGC(JSContext *cx)
{
    js_DestroyContext(cx, JSDCM_NO_GC);
}

JS_PUBLIC_API(void)
JS_DestroyContextMaybeGC(JSContext *cx)
{
    js_DestroyContext(cx, JSDCM_MAYBE_GC);
}

JS_PUBLIC_API(void *)
JS_GetContextPrivate(JSContext *cx)
{
    return cx->data;
}

JS_PUBLIC_API(void)
JS_SetContextPrivate(JSContext *cx, void *data)
{
    cx->data = data;
}

JS_PUBLIC_API(JSRuntime *)
JS_GetRuntime(JSContext *cx)
{
    return cx->runtime;
}

JS_PUBLIC_API(JSContext *)
JS_ContextIterator(JSRuntime *rt, JSContext **iterp)
{
    return js_ContextIterator(rt, JS_TRUE, iterp);
}

JS_PUBLIC_API(JSVersion)
JS_GetVersion(JSContext *cx)
{
    return cx->version & JSVERSION_MASK;
}

JS_PUBLIC_API(JSVersion)
JS_SetVersion(JSContext *cx, JSVersion version)
{
    JSVersion oldVersion;

    JS_ASSERT(version != JSVERSION_UNKNOWN);
    JS_ASSERT((version & ~JSVERSION_MASK) == 0);

    oldVersion = cx->version & JSVERSION_MASK;
    if (version == oldVersion)
        return oldVersion;

    /* We no longer support 1.4 or below. */
    if (version != JSVERSION_DEFAULT && version <= JSVERSION_1_4)
        return oldVersion;

    cx->version = (cx->version & ~JSVERSION_MASK) | version;
    js_OnVersionChange(cx);
    return oldVersion;
}

static struct v2smap {
    JSVersion   version;
    const char  *string;
} v2smap[] = {
    {JSVERSION_1_0,     "1.0"},
    {JSVERSION_1_1,     "1.1"},
    {JSVERSION_1_2,     "1.2"},
    {JSVERSION_1_3,     "1.3"},
    {JSVERSION_1_4,     "1.4"},
    {JSVERSION_ECMA_3,  "ECMAv3"},
    {JSVERSION_1_5,     "1.5"},
    {JSVERSION_1_6,     "1.6"},
    {JSVERSION_1_7,     "1.7"},
    {JSVERSION_DEFAULT, js_default_str},
    {JSVERSION_UNKNOWN, NULL},          /* must be last, NULL is sentinel */
};

JS_PUBLIC_API(const char *)
JS_VersionToString(JSVersion version)
{
    int i;

    for (i = 0; v2smap[i].string; i++)
        if (v2smap[i].version == version)
            return v2smap[i].string;
    return "unknown";
}

JS_PUBLIC_API(JSVersion)
JS_StringToVersion(const char *string)
{
    int i;

    for (i = 0; v2smap[i].string; i++)
        if (strcmp(v2smap[i].string, string) == 0)
            return v2smap[i].version;
    return JSVERSION_UNKNOWN;
}

JS_PUBLIC_API(uint32)
JS_GetOptions(JSContext *cx)
{
    return cx->options;
}

#define SYNC_OPTIONS_TO_VERSION(cx)                                           \
    JS_BEGIN_MACRO                                                            \
        if ((cx)->options & JSOPTION_XML)                                     \
            (cx)->version |= JSVERSION_HAS_XML;                               \
        else                                                                  \
            (cx)->version &= ~JSVERSION_HAS_XML;                              \
    JS_END_MACRO

JS_PUBLIC_API(uint32)
JS_SetOptions(JSContext *cx, uint32 options)
{
    uint32 oldopts = cx->options;
    cx->options = options;
    SYNC_OPTIONS_TO_VERSION(cx);
    return oldopts;
}

JS_PUBLIC_API(uint32)
JS_ToggleOptions(JSContext *cx, uint32 options)
{
    uint32 oldopts = cx->options;
    cx->options ^= options;
    SYNC_OPTIONS_TO_VERSION(cx);
    return oldopts;
}

JS_PUBLIC_API(const char *)
JS_GetImplementationVersion(void)
{
    return "JavaScript-C 1.7.0 2007-10-03";
}


JS_PUBLIC_API(JSObject *)
JS_GetGlobalObject(JSContext *cx)
{
    return cx->globalObject;
}

JS_PUBLIC_API(void)
JS_SetGlobalObject(JSContext *cx, JSObject *obj)
{
    cx->globalObject = obj;

#if JS_HAS_XML_SUPPORT
    cx->xmlSettingFlags = 0;
#endif
}

JSObject *
js_InitFunctionAndObjectClasses(JSContext *cx, JSObject *obj)
{
    JSDHashTable *table;
    JSBool resolving;
    JSRuntime *rt;
    JSResolvingKey key;
    JSResolvingEntry *entry;
    JSObject *fun_proto, *obj_proto;

    /* If cx has no global object, use obj so prototypes can be found. */
    if (!cx->globalObject)
        JS_SetGlobalObject(cx, obj);

    /* Record Function and Object in cx->resolvingTable, if we are resolving. */
    table = cx->resolvingTable;
    resolving = (table && table->entryCount);
    rt = cx->runtime;
    key.obj = obj;
    if (resolving) {
        key.id = ATOM_TO_JSID(rt->atomState.classAtoms[JSProto_Function]);
        entry = (JSResolvingEntry *)
                JS_DHashTableOperate(table, &key, JS_DHASH_ADD);
        if (entry && entry->key.obj && (entry->flags & JSRESFLAG_LOOKUP)) {
            /* Already resolving Function, record Object too. */
            JS_ASSERT(entry->key.obj == obj);
            key.id = ATOM_TO_JSID(rt->atomState.classAtoms[JSProto_Object]);
            entry = (JSResolvingEntry *)
                    JS_DHashTableOperate(table, &key, JS_DHASH_ADD);
        }
        if (!entry) {
            JS_ReportOutOfMemory(cx);
            return NULL;
        }
        JS_ASSERT(!entry->key.obj && entry->flags == 0);
        entry->key = key;
        entry->flags = JSRESFLAG_LOOKUP;
    } else {
        key.id = ATOM_TO_JSID(rt->atomState.classAtoms[JSProto_Object]);
        if (!js_StartResolving(cx, &key, JSRESFLAG_LOOKUP, &entry))
            return NULL;

        key.id = ATOM_TO_JSID(rt->atomState.classAtoms[JSProto_Function]);
        if (!js_StartResolving(cx, &key, JSRESFLAG_LOOKUP, &entry)) {
            key.id = ATOM_TO_JSID(rt->atomState.classAtoms[JSProto_Object]);
            JS_DHashTableOperate(table, &key, JS_DHASH_REMOVE);
            return NULL;
        }

        table = cx->resolvingTable;
    }

    /* Initialize the function class first so constructors can be made. */
    fun_proto = js_InitFunctionClass(cx, obj);
    if (!fun_proto)
        goto out;

    /* Initialize the object class next so Object.prototype works. */
    obj_proto = js_InitObjectClass(cx, obj);
    if (!obj_proto) {
        fun_proto = NULL;
        goto out;
    }

    /* Function.prototype and the global object delegate to Object.prototype. */
    OBJ_SET_PROTO(cx, fun_proto, obj_proto);
    if (!OBJ_GET_PROTO(cx, obj))
        OBJ_SET_PROTO(cx, obj, obj_proto);

out:
    /* If resolving, remove the other entry (Object or Function) from table. */
    JS_DHashTableOperate(table, &key, JS_DHASH_REMOVE);
    if (!resolving) {
        /* If not resolving, remove the first entry added above, for Object. */
        JS_ASSERT(key.id ==                                                   \
                  ATOM_TO_JSID(rt->atomState.classAtoms[JSProto_Function]));
        key.id = ATOM_TO_JSID(rt->atomState.classAtoms[JSProto_Object]);
        JS_DHashTableOperate(table, &key, JS_DHASH_REMOVE);
    }
    return fun_proto;
}

JS_PUBLIC_API(JSBool)
JS_InitStandardClasses(JSContext *cx, JSObject *obj)
{
    JSAtom *atom;

    CHECK_REQUEST(cx);

    /* Define a top-level property 'undefined' with the undefined value. */
    atom = cx->runtime->atomState.typeAtoms[JSTYPE_VOID];
    if (!OBJ_DEFINE_PROPERTY(cx, obj, ATOM_TO_JSID(atom), JSVAL_VOID,
                             NULL, NULL, JSPROP_PERMANENT, NULL)) {
        return JS_FALSE;
    }

    /* Function and Object require cooperative bootstrapping magic. */
    if (!js_InitFunctionAndObjectClasses(cx, obj))
        return JS_FALSE;

    /* Initialize the rest of the standard objects and functions. */
    return js_InitArrayClass(cx, obj) &&
           js_InitBlockClass(cx, obj) &&
           js_InitBooleanClass(cx, obj) &&
           js_InitCallClass(cx, obj) &&
           js_InitExceptionClasses(cx, obj) &&
           js_InitMathClass(cx, obj) &&
           js_InitNumberClass(cx, obj) &&
           js_InitRegExpClass(cx, obj) &&
           js_InitStringClass(cx, obj) &&
#if JS_HAS_SCRIPT_OBJECT
           js_InitScriptClass(cx, obj) &&
#endif
#if JS_HAS_XML_SUPPORT
           js_InitXMLClasses(cx, obj) &&
#endif
#if JS_HAS_FILE_OBJECT
           js_InitFileClass(cx, obj) &&
#endif
#if JS_HAS_GENERATORS
           js_InitIteratorClasses(cx, obj) &&
#endif
           js_InitDateClass(cx, obj);
}

#define ATOM_OFFSET(name)       offsetof(JSAtomState,name##Atom)
#define CLASS_ATOM_OFFSET(name) offsetof(JSAtomState,classAtoms[JSProto_##name])
#define OFFSET_TO_ATOM(rt,off)  (*(JSAtom **)((char*)&(rt)->atomState + (off)))
#define CLASP(name)             (JSClass *)&js_##name##Class

#define EAGER_ATOM(name)            ATOM_OFFSET(name), NULL
#define EAGER_CLASS_ATOM(name)      CLASS_ATOM_OFFSET(name), NULL
#define EAGER_ATOM_AND_CLASP(name)  EAGER_CLASS_ATOM(name), CLASP(name)
#define LAZY_ATOM(name)             ATOM_OFFSET(lazy.name), js_##name##_str

typedef struct JSStdName {
    JSObjectOp  init;
    size_t      atomOffset;     /* offset of atom pointer in JSAtomState */
    const char  *name;          /* null if atom is pre-pinned, else name */
    JSClass     *clasp;
} JSStdName;

static JSAtom *
StdNameToAtom(JSContext *cx, JSStdName *stdn)
{
    size_t offset;
    JSAtom *atom;
    const char *name;

    offset = stdn->atomOffset;
    atom = OFFSET_TO_ATOM(cx->runtime, offset);
    if (!atom) {
        name = stdn->name;
        if (name) {
            atom = js_Atomize(cx, name, strlen(name), ATOM_PINNED);
            OFFSET_TO_ATOM(cx->runtime, offset) = atom;
        }
    }
    return atom;
}

/*
 * Table of class initializers and their atom offsets in rt->atomState.
 * If you add a "standard" class, remember to update this table.
 */
static JSStdName standard_class_atoms[] = {
    {js_InitFunctionAndObjectClasses,   EAGER_ATOM_AND_CLASP(Function)},
    {js_InitFunctionAndObjectClasses,   EAGER_ATOM_AND_CLASP(Object)},
    {js_InitArrayClass,                 EAGER_ATOM_AND_CLASP(Array)},
    {js_InitBlockClass,                 EAGER_ATOM_AND_CLASP(Block)},
    {js_InitBooleanClass,               EAGER_ATOM_AND_CLASP(Boolean)},
    {js_InitDateClass,                  EAGER_ATOM_AND_CLASP(Date)},
    {js_InitMathClass,                  EAGER_ATOM_AND_CLASP(Math)},
    {js_InitNumberClass,                EAGER_ATOM_AND_CLASP(Number)},
    {js_InitStringClass,                EAGER_ATOM_AND_CLASP(String)},
    {js_InitCallClass,                  EAGER_ATOM_AND_CLASP(Call)},
    {js_InitExceptionClasses,           EAGER_ATOM_AND_CLASP(Error)},
    {js_InitRegExpClass,                EAGER_ATOM_AND_CLASP(RegExp)},
#if JS_HAS_SCRIPT_OBJECT
    {js_InitScriptClass,                EAGER_ATOM_AND_CLASP(Script)},
#endif
#if JS_HAS_XML_SUPPORT
    {js_InitXMLClass,                   EAGER_ATOM_AND_CLASP(XML)},
    {js_InitNamespaceClass,             EAGER_ATOM_AND_CLASP(Namespace)},
    {js_InitQNameClass,                 EAGER_ATOM_AND_CLASP(QName)},
#endif
#if JS_HAS_FILE_OBJECT
    {js_InitFileClass,                  EAGER_ATOM_AND_CLASP(File)},
#endif
#if JS_HAS_GENERATORS
    {js_InitIteratorClasses,            EAGER_ATOM_AND_CLASP(StopIteration)},
#endif
    {NULL,                              0, NULL, NULL}
};

/*
 * Table of top-level function and constant names and their init functions.
 * If you add a "standard" global function or property, remember to update
 * this table.
 */
static JSStdName standard_class_names[] = {
    /* ECMA requires that eval be a direct property of the global object. */
    {js_InitObjectClass,        EAGER_ATOM(eval), NULL},

    /* Global properties and functions defined by the Number class. */
    {js_InitNumberClass,        LAZY_ATOM(NaN), NULL},
    {js_InitNumberClass,        LAZY_ATOM(Infinity), NULL},
    {js_InitNumberClass,        LAZY_ATOM(isNaN), NULL},
    {js_InitNumberClass,        LAZY_ATOM(isFinite), NULL},
    {js_InitNumberClass,        LAZY_ATOM(parseFloat), NULL},
    {js_InitNumberClass,        LAZY_ATOM(parseInt), NULL},

    /* String global functions. */
    {js_InitStringClass,        LAZY_ATOM(escape), NULL},
    {js_InitStringClass,        LAZY_ATOM(unescape), NULL},
    {js_InitStringClass,        LAZY_ATOM(decodeURI), NULL},
    {js_InitStringClass,        LAZY_ATOM(encodeURI), NULL},
    {js_InitStringClass,        LAZY_ATOM(decodeURIComponent), NULL},
    {js_InitStringClass,        LAZY_ATOM(encodeURIComponent), NULL},
#if JS_HAS_UNEVAL
    {js_InitStringClass,        LAZY_ATOM(uneval), NULL},
#endif

    /* Exception constructors. */
    {js_InitExceptionClasses,   EAGER_CLASS_ATOM(Error), CLASP(Error)},
    {js_InitExceptionClasses,   EAGER_CLASS_ATOM(InternalError), CLASP(Error)},
    {js_InitExceptionClasses,   EAGER_CLASS_ATOM(EvalError), CLASP(Error)},
    {js_InitExceptionClasses,   EAGER_CLASS_ATOM(RangeError), CLASP(Error)},
    {js_InitExceptionClasses,   EAGER_CLASS_ATOM(ReferenceError), CLASP(Error)},
    {js_InitExceptionClasses,   EAGER_CLASS_ATOM(SyntaxError), CLASP(Error)},
    {js_InitExceptionClasses,   EAGER_CLASS_ATOM(TypeError), CLASP(Error)},
    {js_InitExceptionClasses,   EAGER_CLASS_ATOM(URIError), CLASP(Error)},

#if JS_HAS_XML_SUPPORT
    {js_InitAnyNameClass,       EAGER_ATOM_AND_CLASP(AnyName)},
    {js_InitAttributeNameClass, EAGER_ATOM_AND_CLASP(AttributeName)},
    {js_InitXMLClass,           LAZY_ATOM(XMLList), &js_XMLClass},
    {js_InitXMLClass,           LAZY_ATOM(isXMLName), NULL},
#endif

#if JS_HAS_GENERATORS
    {js_InitIteratorClasses,    EAGER_ATOM_AND_CLASP(Iterator)},
    {js_InitIteratorClasses,    EAGER_ATOM_AND_CLASP(Generator)},
#endif

    {NULL,                      0, NULL, NULL}
};

static JSStdName object_prototype_names[] = {
    /* Object.prototype properties (global delegates to Object.prototype). */
    {js_InitObjectClass,        EAGER_ATOM(proto), NULL},
    {js_InitObjectClass,        EAGER_ATOM(parent), NULL},
    {js_InitObjectClass,        EAGER_ATOM(count), NULL},
#if JS_HAS_TOSOURCE
    {js_InitObjectClass,        EAGER_ATOM(toSource), NULL},
#endif
    {js_InitObjectClass,        EAGER_ATOM(toString), NULL},
    {js_InitObjectClass,        EAGER_ATOM(toLocaleString), NULL},
    {js_InitObjectClass,        EAGER_ATOM(valueOf), NULL},
#if JS_HAS_OBJ_WATCHPOINT
    {js_InitObjectClass,        LAZY_ATOM(watch), NULL},
    {js_InitObjectClass,        LAZY_ATOM(unwatch), NULL},
#endif
    {js_InitObjectClass,        LAZY_ATOM(hasOwnProperty), NULL},
    {js_InitObjectClass,        LAZY_ATOM(isPrototypeOf), NULL},
    {js_InitObjectClass,        LAZY_ATOM(propertyIsEnumerable), NULL},
#if JS_HAS_GETTER_SETTER
    {js_InitObjectClass,        LAZY_ATOM(defineGetter), NULL},
    {js_InitObjectClass,        LAZY_ATOM(defineSetter), NULL},
    {js_InitObjectClass,        LAZY_ATOM(lookupGetter), NULL},
    {js_InitObjectClass,        LAZY_ATOM(lookupSetter), NULL},
#endif

    {NULL,                      0, NULL, NULL}
};

JS_PUBLIC_API(JSBool)
JS_ResolveStandardClass(JSContext *cx, JSObject *obj, jsval id,
                        JSBool *resolved)
{
    JSString *idstr;
    JSRuntime *rt;
    JSAtom *atom;
    JSStdName *stdnm;
    uintN i;

    CHECK_REQUEST(cx);
    *resolved = JS_FALSE;

    if (!JSVAL_IS_STRING(id))
        return JS_TRUE;
    idstr = JSVAL_TO_STRING(id);
    rt = cx->runtime;

    /* Check whether we're resolving 'undefined', and define it if so. */
    atom = rt->atomState.typeAtoms[JSTYPE_VOID];
    if (idstr == ATOM_TO_STRING(atom)) {
        *resolved = JS_TRUE;
        return OBJ_DEFINE_PROPERTY(cx, obj, ATOM_TO_JSID(atom), JSVAL_VOID,
                                   NULL, NULL, JSPROP_PERMANENT, NULL);
    }

    /* Try for class constructors/prototypes named by well-known atoms. */
    stdnm = NULL;
    for (i = 0; standard_class_atoms[i].init; i++) {
        atom = OFFSET_TO_ATOM(rt, standard_class_atoms[i].atomOffset);
        if (idstr == ATOM_TO_STRING(atom)) {
            stdnm = &standard_class_atoms[i];
            break;
        }
    }

    if (!stdnm) {
        /* Try less frequently used top-level functions and constants. */
        for (i = 0; standard_class_names[i].init; i++) {
            atom = StdNameToAtom(cx, &standard_class_names[i]);
            if (!atom)
                return JS_FALSE;
            if (idstr == ATOM_TO_STRING(atom)) {
                stdnm = &standard_class_names[i];
                break;
            }
        }

        if (!stdnm && !OBJ_GET_PROTO(cx, obj)) {
            /*
             * Try even less frequently used names delegated from the global
             * object to Object.prototype, but only if the Object class hasn't
             * yet been initialized.
             */
            for (i = 0; object_prototype_names[i].init; i++) {
                atom = StdNameToAtom(cx, &object_prototype_names[i]);
                if (!atom)
                    return JS_FALSE;
                if (idstr == ATOM_TO_STRING(atom)) {
                    stdnm = &standard_class_names[i];
                    break;
                }
            }
        }
    }

    if (stdnm) {
        /*
         * If this standard class is anonymous and obj advertises itself as a
         * global object (in order to reserve slots for standard class object
         * pointers), then we don't want to resolve by name.
         *
         * If inversely, either id does not name a class, or id does not name
         * an anonymous class, or the global does not reserve slots for class
         * objects, then we must call the init hook here.
         */
        if (stdnm->clasp &&
            (stdnm->clasp->flags & JSCLASS_IS_ANONYMOUS) &&
            (OBJ_GET_CLASS(cx, obj)->flags & JSCLASS_IS_GLOBAL)) {
            return JS_TRUE;
        }

        if (!stdnm->init(cx, obj))
            return JS_FALSE;
        *resolved = JS_TRUE;
    }
    return JS_TRUE;
}

static JSBool
AlreadyHasOwnProperty(JSContext *cx, JSObject *obj, JSAtom *atom)
{
    JSScopeProperty *sprop;
    JSScope *scope;

    JS_ASSERT(OBJ_IS_NATIVE(obj));
    JS_LOCK_OBJ(cx, obj);
    scope = OBJ_SCOPE(obj);
    sprop = SCOPE_GET_PROPERTY(scope, ATOM_TO_JSID(atom));
    JS_UNLOCK_SCOPE(cx, scope);
    return sprop != NULL;
}

JS_PUBLIC_API(JSBool)
JS_EnumerateStandardClasses(JSContext *cx, JSObject *obj)
{
    JSRuntime *rt;
    JSAtom *atom;
    uintN i;

    CHECK_REQUEST(cx);
    rt = cx->runtime;

    /* Check whether we need to bind 'undefined' and define it if so. */
    atom = rt->atomState.typeAtoms[JSTYPE_VOID];
    if (!AlreadyHasOwnProperty(cx, obj, atom) &&
        !OBJ_DEFINE_PROPERTY(cx, obj, ATOM_TO_JSID(atom), JSVAL_VOID,
                             NULL, NULL, JSPROP_PERMANENT, NULL)) {
        return JS_FALSE;
    }

    /* Initialize any classes that have not been resolved yet. */
    for (i = 0; standard_class_atoms[i].init; i++) {
        atom = OFFSET_TO_ATOM(rt, standard_class_atoms[i].atomOffset);
        if (!AlreadyHasOwnProperty(cx, obj, atom) &&
            !standard_class_atoms[i].init(cx, obj)) {
            return JS_FALSE;
        }
    }

    return JS_TRUE;
}

static JSIdArray *
AddAtomToArray(JSContext *cx, JSAtom *atom, JSIdArray *ida, jsint *ip)
{
    jsint i, length;

    i = *ip;
    length = ida->length;
    if (i >= length) {
        ida = js_SetIdArrayLength(cx, ida, JS_MAX(length * 2, 8));
        if (!ida)
            return NULL;
        JS_ASSERT(i < ida->length);
    }
    ida->vector[i] = ATOM_TO_JSID(atom);
    *ip = i + 1;
    return ida;
}

static JSIdArray *
EnumerateIfResolved(JSContext *cx, JSObject *obj, JSAtom *atom, JSIdArray *ida,
                    jsint *ip, JSBool *foundp)
{
    *foundp = AlreadyHasOwnProperty(cx, obj, atom);
    if (*foundp)
        ida = AddAtomToArray(cx, atom, ida, ip);
    return ida;
}

JS_PUBLIC_API(JSIdArray *)
JS_EnumerateResolvedStandardClasses(JSContext *cx, JSObject *obj,
                                    JSIdArray *ida)
{
    JSRuntime *rt;
    jsint i, j, k;
    JSAtom *atom;
    JSBool found;
    JSObjectOp init;

    CHECK_REQUEST(cx);
    rt = cx->runtime;
    if (ida) {
        i = ida->length;
    } else {
        ida = js_NewIdArray(cx, 8);
        if (!ida)
            return NULL;
        i = 0;
    }

    /* Check whether 'undefined' has been resolved and enumerate it if so. */
    atom = rt->atomState.typeAtoms[JSTYPE_VOID];
    ida = EnumerateIfResolved(cx, obj, atom, ida, &i, &found);
    if (!ida)
        return NULL;

    /* Enumerate only classes that *have* been resolved. */
    for (j = 0; standard_class_atoms[j].init; j++) {
        atom = OFFSET_TO_ATOM(rt, standard_class_atoms[j].atomOffset);
        ida = EnumerateIfResolved(cx, obj, atom, ida, &i, &found);
        if (!ida)
            return NULL;

        if (found) {
            init = standard_class_atoms[j].init;

            for (k = 0; standard_class_names[k].init; k++) {
                if (standard_class_names[k].init == init) {
                    atom = StdNameToAtom(cx, &standard_class_names[k]);
                    ida = AddAtomToArray(cx, atom, ida, &i);
                    if (!ida)
                        return NULL;
                }
            }

            if (init == js_InitObjectClass) {
                for (k = 0; object_prototype_names[k].init; k++) {
                    atom = StdNameToAtom(cx, &object_prototype_names[k]);
                    ida = AddAtomToArray(cx, atom, ida, &i);
                    if (!ida)
                        return NULL;
                }
            }
        }
    }

    /* Trim to exact length via js_SetIdArrayLength. */
    return js_SetIdArrayLength(cx, ida, i);
}

#undef ATOM_OFFSET
#undef CLASS_ATOM_OFFSET
#undef OFFSET_TO_ATOM
#undef CLASP

#undef EAGER_ATOM
#undef EAGER_CLASS_ATOM
#undef EAGER_ATOM_CLASP
#undef LAZY_ATOM

JS_PUBLIC_API(JSBool)
JS_GetClassObject(JSContext *cx, JSObject *obj, JSProtoKey key,
                  JSObject **objp)
{
    CHECK_REQUEST(cx);
    return js_GetClassObject(cx, obj, key, objp);
}

JS_PUBLIC_API(JSObject *)
JS_GetScopeChain(JSContext *cx)
{
    JSStackFrame *fp;

    fp = cx->fp;
    if (!fp) {
        JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_INACTIVE);
        return NULL;
    }
    return js_GetScopeChain(cx, fp);
}

JS_PUBLIC_API(void *)
JS_malloc(JSContext *cx, size_t nbytes)
{
    void *p;

    JS_ASSERT(nbytes != 0);
    if (nbytes == 0)
        nbytes = 1;

    p = malloc(nbytes);
    if (!p) {
        JS_ReportOutOfMemory(cx);
        return NULL;
    }
    js_UpdateMallocCounter(cx, nbytes);

    return p;
}

JS_PUBLIC_API(void *)
JS_realloc(JSContext *cx, void *p, size_t nbytes)
{
    p = realloc(p, nbytes);
    if (!p)
        JS_ReportOutOfMemory(cx);
    return p;
}

JS_PUBLIC_API(void)
JS_free(JSContext *cx, void *p)
{
    if (p)
        free(p);
}

JS_PUBLIC_API(char *)
JS_strdup(JSContext *cx, const char *s)
{
    size_t n;
    void *p;

    n = strlen(s) + 1;
    p = JS_malloc(cx, n);
    if (!p)
        return NULL;
    return (char *)memcpy(p, s, n);
}

JS_PUBLIC_API(jsdouble *)
JS_NewDouble(JSContext *cx, jsdouble d)
{
    CHECK_REQUEST(cx);
    return js_NewDouble(cx, d, 0);
}

JS_PUBLIC_API(JSBool)
JS_NewDoubleValue(JSContext *cx, jsdouble d, jsval *rval)
{
    CHECK_REQUEST(cx);
    return js_NewDoubleValue(cx, d, rval);
}

JS_PUBLIC_API(JSBool)
JS_NewNumberValue(JSContext *cx, jsdouble d, jsval *rval)
{
    CHECK_REQUEST(cx);
    return js_NewNumberValue(cx, d, rval);
}

#undef JS_AddRoot
JS_PUBLIC_API(JSBool)
JS_AddRoot(JSContext *cx, void *rp)
{
    CHECK_REQUEST(cx);
    return js_AddRoot(cx, rp, NULL);
}

JS_PUBLIC_API(JSBool)
JS_AddNamedRootRT(JSRuntime *rt, void *rp, const char *name)
{
    return js_AddRootRT(rt, rp, name);
}

JS_PUBLIC_API(JSBool)
JS_RemoveRoot(JSContext *cx, void *rp)
{
    CHECK_REQUEST(cx);
    return js_RemoveRoot(cx->runtime, rp);
}

JS_PUBLIC_API(JSBool)
JS_RemoveRootRT(JSRuntime *rt, void *rp)
{
    return js_RemoveRoot(rt, rp);
}

JS_PUBLIC_API(JSBool)
JS_AddNamedRoot(JSContext *cx, void *rp, const char *name)
{
    CHECK_REQUEST(cx);
    return js_AddRoot(cx, rp, name);
}

JS_PUBLIC_API(void)
JS_ClearNewbornRoots(JSContext *cx)
{
    JS_CLEAR_WEAK_ROOTS(&cx->weakRoots);
}

JS_PUBLIC_API(JSBool)
JS_EnterLocalRootScope(JSContext *cx)
{
    CHECK_REQUEST(cx);
    return js_EnterLocalRootScope(cx);
}

JS_PUBLIC_API(void)
JS_LeaveLocalRootScope(JSContext *cx)
{
    CHECK_REQUEST(cx);
    js_LeaveLocalRootScope(cx);
}

JS_PUBLIC_API(void)
JS_LeaveLocalRootScopeWithResult(JSContext *cx, jsval rval)
{
    CHECK_REQUEST(cx);
    js_LeaveLocalRootScopeWithResult(cx, rval);
}

JS_PUBLIC_API(void)
JS_ForgetLocalRoot(JSContext *cx, void *thing)
{
    CHECK_REQUEST(cx);
    js_ForgetLocalRoot(cx, (jsval) thing);
}

#ifdef DEBUG

JS_PUBLIC_API(void)
JS_DumpNamedRoots(JSRuntime *rt,
                  void (*dump)(const char *name, void *rp, void *data),
                  void *data)
{
    js_DumpNamedRoots(rt, dump, data);
}

#endif /* DEBUG */

JS_PUBLIC_API(uint32)
JS_MapGCRoots(JSRuntime *rt, JSGCRootMapFun map, void *data)
{
    return js_MapGCRoots(rt, map, data);
}

JS_PUBLIC_API(JSBool)
JS_LockGCThing(JSContext *cx, void *thing)
{
    JSBool ok;

    CHECK_REQUEST(cx);
    ok = js_LockGCThing(cx, thing);
    if (!ok)
        JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_CANT_LOCK);
    return ok;
}

JS_PUBLIC_API(JSBool)
JS_LockGCThingRT(JSRuntime *rt, void *thing)
{
    return js_LockGCThingRT(rt, thing);
}

JS_PUBLIC_API(JSBool)
JS_UnlockGCThing(JSContext *cx, void *thing)
{
    JSBool ok;

    CHECK_REQUEST(cx);
    ok = js_UnlockGCThingRT(cx->runtime, thing);
    if (!ok)
        JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_CANT_UNLOCK);
    return ok;
}

JS_PUBLIC_API(JSBool)
JS_UnlockGCThingRT(JSRuntime *rt, void *thing)
{
    return js_UnlockGCThingRT(rt, thing);
}

JS_PUBLIC_API(void)
JS_MarkGCThing(JSContext *cx, void *thing, const char *name, void *arg)
{
    JS_ASSERT(cx->runtime->gcLevel > 0);
#ifdef JS_THREADSAFE
    JS_ASSERT(cx->runtime->gcThread->id == js_CurrentThreadId());
#endif

    GC_MARK(cx, thing, name);
}

JS_PUBLIC_API(void)
JS_GC(JSContext *cx)
{
#if JS_HAS_GENERATORS
    /* Run previously scheduled but delayed close hooks. */
    js_RunCloseHooks(cx);
#endif

    /* Don't nuke active arenas if executing or compiling. */
    if (cx->stackPool.current == &cx->stackPool.first)
        JS_FinishArenaPool(&cx->stackPool);
    if (cx->tempPool.current == &cx->tempPool.first)
        JS_FinishArenaPool(&cx->tempPool);
    js_GC(cx, GC_NORMAL);

#if JS_HAS_GENERATORS
    /*
     * Run close hooks for objects that became unreachable after the last GC.
     */
    js_RunCloseHooks(cx);
#endif
}

JS_PUBLIC_API(void)
JS_MaybeGC(JSContext *cx)
{
#ifdef WAY_TOO_MUCH_GC
    JS_GC(cx);
#else
    JSRuntime *rt;
    uint32 bytes, lastBytes;

    rt = cx->runtime;
    bytes = rt->gcBytes;
    lastBytes = rt->gcLastBytes;

    /*
     * We run the GC if we used all available free GC cells and had to
     * allocate extra 1/5 of GC arenas since the last run of GC, or if
     * we have malloc'd more bytes through JS_malloc than we were told
     * to allocate by JS_NewRuntime.
     *
     * The reason for
     *   bytes > 6/5 lastBytes
     * condition is the following. Bug 312238 changed bytes and lastBytes
     * to mean the total amount of memory that the GC uses now and right
     * after the last GC.
     *
     * Before the bug the variables meant the size of allocated GC things
     * now and right after the last GC. That size did not include the
     * memory taken by free GC cells and the condition was
     *   bytes > 3/2 lastBytes.
     * That is, we run the GC if we have half again as many bytes of
     * GC-things as the last time we GC'd. To be compatible we need to
     * express that condition through the new meaning of bytes and
     * lastBytes.
     *
     * We write the original condition as
     *   B*(1-F) > 3/2 Bl*(1-Fl)
     * where B is the total memory size allocated by GC and F is the free
     * cell density currently and Sl and Fl are the size and the density
     * right after GC. The density by definition is memory taken by free
     * cells divided by total amount of memory. In other words, B and Bl
     * are bytes and lastBytes with the new meaning and B*(1-F) and
     * Bl*(1-Fl) are bytes and lastBytes with the original meaning.
     *
     * Our task is to exclude F and Fl from the last statement. According
     * the stats from bug 331770 Fl is about 20-30% for GC allocations
     * that contribute to S and Sl for a typical run of the browser. It
     * means that the original condition implied that we did not run GC
     * unless we exhausted the pool of free cells. Indeed if we still
     * have free cells, then B == Bl since we did not yet allocated any
     * new arenas and the condition means
     *   1 - F > 3/2 (1-Fl) or 3/2Fl > 1/2 + F
     * That implies 3/2 Fl > 1/2 or Fl > 1/3. That can not be fulfilled
     * for the state described by the stats. So we can write the original
     * condition as:
     *   F == 0 && B > 3/2 Bl(1-Fl)
     * Again using the stats we see that Fl is about 20% when the browser
     * starts up and when we are far from hitting rt->gcMaxBytes. With
     * this F we have
     * F == 0 && B > 3/2 Bl(1-0.8) or just B > 6/5 Bl.
     */
    if ((bytes > 8192 && bytes > lastBytes + lastBytes / 5) ||
        rt->gcMallocBytes >= rt->gcMaxMallocBytes) {
        JS_GC(cx);
    }
#if JS_HAS_GENERATORS
    else {
        /* Run scheduled but not yet executed close hooks. */
        js_RunCloseHooks(cx);
    }
#endif
#endif
}

JS_PUBLIC_API(JSGCCallback)
JS_SetGCCallback(JSContext *cx, JSGCCallback cb)
{
    return JS_SetGCCallbackRT(cx->runtime, cb);
}

JS_PUBLIC_API(JSGCCallback)
JS_SetGCCallbackRT(JSRuntime *rt, JSGCCallback cb)
{
    JSGCCallback oldcb;

    oldcb = rt->gcCallback;
    rt->gcCallback = cb;
    return oldcb;
}

JS_PUBLIC_API(JSBool)
JS_IsAboutToBeFinalized(JSContext *cx, void *thing)
{
    JS_ASSERT(thing);
    return js_IsAboutToBeFinalized(cx, thing);
}

JS_PUBLIC_API(void)
JS_SetGCParameter(JSRuntime *rt, JSGCParamKey key, uint32 value)
{
    switch (key) {
      case JSGC_MAX_BYTES:
        rt->gcMaxBytes = value;
        break;
      case JSGC_MAX_MALLOC_BYTES:
        rt->gcMaxMallocBytes = value;
        break;
    }
}

JS_PUBLIC_API(intN)
JS_AddExternalStringFinalizer(JSStringFinalizeOp finalizer)
{
    return js_ChangeExternalStringFinalizer(NULL, finalizer);
}

JS_PUBLIC_API(intN)
JS_RemoveExternalStringFinalizer(JSStringFinalizeOp finalizer)
{
    return js_ChangeExternalStringFinalizer(finalizer, NULL);
}

JS_PUBLIC_API(JSString *)
JS_NewExternalString(JSContext *cx, jschar *chars, size_t length, intN type)
{
    JSString *str;

    CHECK_REQUEST(cx);
    JS_ASSERT(GCX_EXTERNAL_STRING <= type && type < (intN) GCX_NTYPES);

    str = (JSString *) js_NewGCThing(cx, (uintN) type, sizeof(JSString));
    if (!str)
        return NULL;
    str->length = length;
    str->chars = chars;
    return str;
}

JS_PUBLIC_API(intN)
JS_GetExternalStringGCType(JSRuntime *rt, JSString *str)
{
    uint8 type = (uint8) (*js_GetGCThingFlags(str) & GCF_TYPEMASK);

    if (type >= GCX_EXTERNAL_STRING)
        return (intN)type;
    JS_ASSERT(type == GCX_STRING || type == GCX_MUTABLE_STRING);
    return -1;
}

JS_PUBLIC_API(void)
JS_SetThreadStackLimit(JSContext *cx, jsuword limitAddr)
{
#if JS_STACK_GROWTH_DIRECTION > 0
    if (limitAddr == 0)
        limitAddr = (jsuword)-1;
#endif
    cx->stackLimit = limitAddr;
}

/************************************************************************/

JS_PUBLIC_API(void)
JS_DestroyIdArray(JSContext *cx, JSIdArray *ida)
{
    JS_free(cx, ida);
}

JS_PUBLIC_API(JSBool)
JS_ValueToId(JSContext *cx, jsval v, jsid *idp)
{
    JSAtom *atom;

    CHECK_REQUEST(cx);
    if (JSVAL_IS_INT(v)) {
        *idp = INT_JSVAL_TO_JSID(v);
    } else {
#if JS_HAS_XML_SUPPORT
        if (JSVAL_IS_OBJECT(v)) {
            *idp = OBJECT_JSVAL_TO_JSID(v);
            return JS_TRUE;
        }
#endif
        atom = js_ValueToStringAtom(cx, v);
        if (!atom)
            return JS_FALSE;
        *idp = ATOM_TO_JSID(atom);
    }
    return JS_TRUE;
}

JS_PUBLIC_API(JSBool)
JS_IdToValue(JSContext *cx, jsid id, jsval *vp)
{
    CHECK_REQUEST(cx);
    *vp = ID_TO_VALUE(id);
    return JS_TRUE;
}

JS_PUBLIC_API(JSBool)
JS_PropertyStub(JSContext *cx, JSObject *obj, jsval id, jsval *vp)
{
    return JS_TRUE;
}

JS_PUBLIC_API(JSBool)
JS_EnumerateStub(JSContext *cx, JSObject *obj)
{
    return JS_TRUE;
}

JS_PUBLIC_API(JSBool)
JS_ResolveStub(JSContext *cx, JSObject *obj, jsval id)
{
    return JS_TRUE;
}

JS_PUBLIC_API(JSBool)
JS_ConvertStub(JSContext *cx, JSObject *obj, JSType type, jsval *vp)
{
    return js_TryValueOf(cx, obj, type, vp);
}

JS_PUBLIC_API(void)
JS_FinalizeStub(JSContext *cx, JSObject *obj)
{
}

JS_PUBLIC_API(JSObject *)
JS_InitClass(JSContext *cx, JSObject *obj, JSObject *parent_proto,
             JSClass *clasp, JSNative constructor, uintN nargs,
             JSPropertySpec *ps, JSFunctionSpec *fs,
             JSPropertySpec *static_ps, JSFunctionSpec *static_fs)
{
    JSAtom *atom;
    JSProtoKey key;
    JSObject *proto, *ctor;
    JSTempValueRooter tvr;
    jsval cval, rval;
    JSBool named;
    JSFunction *fun;

    CHECK_REQUEST(cx);
    atom = js_Atomize(cx, clasp->name, strlen(clasp->name), 0);
    if (!atom)
        return NULL;

    /*
     * When initializing a standard class, if no parent_proto (grand-proto of
     * instances of the class, parent-proto of the class's prototype object)
     * is given, we must use Object.prototype if it is available.  Otherwise,
     * we could look up the wrong binding for a class name in obj.  Example:
     *
     *   String = Array;
     *   print("hi there".join);
     *
     * should print undefined, not Array.prototype.join.  This is required by
     * ECMA-262, alas.  It might have been better to make String readonly and
     * permanent in the global object, instead -- but that's too big a change
     * to swallow at this point.
     */
    key = JSCLASS_CACHED_PROTO_KEY(clasp);
    if (key != JSProto_Null &&
        !parent_proto &&
        !js_GetClassPrototype(cx, obj, INT_TO_JSID(JSProto_Object),
                              &parent_proto)) {
        return NULL;
    }

    /* Create a prototype object for this class. */
    proto = js_NewObject(cx, clasp, parent_proto, obj);
    if (!proto)
        return NULL;

    /* After this point, control must exit via label bad or out. */
    JS_PUSH_TEMP_ROOT_OBJECT(cx, proto, &tvr);

    if (!constructor) {
        /*
         * Lacking a constructor, name the prototype (e.g., Math) unless this
         * class (a) is anonymous, i.e. for internal use only; (b) the class
         * of obj (the global object) is has a reserved slot indexed by key;
         * and (c) key is not the null key.
         */
        if ((clasp->flags & JSCLASS_IS_ANONYMOUS) &&
            (OBJ_GET_CLASS(cx, obj)->flags & JSCLASS_IS_GLOBAL) &&
            key != JSProto_Null) {
            named = JS_FALSE;
        } else {
            named = OBJ_DEFINE_PROPERTY(cx, obj, ATOM_TO_JSID(atom),
                                        OBJECT_TO_JSVAL(proto),
                                        NULL, NULL,
                                        (clasp->flags & JSCLASS_IS_ANONYMOUS)
                                        ? JSPROP_READONLY | JSPROP_PERMANENT
                                        : 0,
                                        NULL);
            if (!named)
                goto bad;
        }

        ctor = proto;
    } else {
        /* Define the constructor function in obj's scope. */
        fun = js_DefineFunction(cx, obj, atom, constructor, nargs, 0);
        named = (fun != NULL);
        if (!fun)
            goto bad;

        /*
         * Remember the class this function is a constructor for so that
         * we know to create an object of this class when we call the
         * constructor.
         */
        fun->clasp = clasp;

        /*
         * Optionally construct the prototype object, before the class has
         * been fully initialized.  Allow the ctor to replace proto with a
         * different object, as is done for operator new -- and as at least
         * XML support requires.
         */
        ctor = fun->object;
        if (clasp->flags & JSCLASS_CONSTRUCT_PROTOTYPE) {
            cval = OBJECT_TO_JSVAL(ctor);
            if (!js_InternalConstruct(cx, proto, cval, 0, NULL, &rval))
                goto bad;
            if (!JSVAL_IS_PRIMITIVE(rval) && JSVAL_TO_OBJECT(rval) != proto)
                proto = JSVAL_TO_OBJECT(rval);
        }

        /* Connect constructor and prototype by named properties. */
        if (!js_SetClassPrototype(cx, ctor, proto,
                                  JSPROP_READONLY | JSPROP_PERMANENT)) {
            goto bad;
        }

        /* Bootstrap Function.prototype (see also JS_InitStandardClasses). */
        if (OBJ_GET_CLASS(cx, ctor) == clasp) {
            JS_ASSERT(!OBJ_GET_PROTO(cx, ctor));
            OBJ_SET_PROTO(cx, ctor, proto);
        }
    }

    /* Add properties and methods to the prototype and the constructor. */
    if ((ps && !JS_DefineProperties(cx, proto, ps)) ||
        (fs && !JS_DefineFunctions(cx, proto, fs)) ||
        (static_ps && !JS_DefineProperties(cx, ctor, static_ps)) ||
        (static_fs && !JS_DefineFunctions(cx, ctor, static_fs))) {
        goto bad;
    }

    /* If this is a standard class, cache its prototype. */
    if (key != JSProto_Null && !js_SetClassObject(cx, obj, key, ctor))
        goto bad;

out:
    JS_POP_TEMP_ROOT(cx, &tvr);
    return proto;

bad:
    if (named)
        (void) OBJ_DELETE_PROPERTY(cx, obj, ATOM_TO_JSID(atom), &rval);
    proto = NULL;
    goto out;
}

#ifdef JS_THREADSAFE
JS_PUBLIC_API(JSClass *)
JS_GetClass(JSContext *cx, JSObject *obj)
{
    return (JSClass *)
        JSVAL_TO_PRIVATE(GC_AWARE_GET_SLOT(cx, obj, JSSLOT_CLASS));
}
#else
JS_PUBLIC_API(JSClass *)
JS_GetClass(JSObject *obj)
{
    return LOCKED_OBJ_GET_CLASS(obj);
}
#endif

JS_PUBLIC_API(JSBool)
JS_InstanceOf(JSContext *cx, JSObject *obj, JSClass *clasp, jsval *argv)
{
    JSFunction *fun;

    CHECK_REQUEST(cx);
    if (OBJ_GET_CLASS(cx, obj) == clasp)
        return JS_TRUE;
    if (argv) {
        fun = js_ValueToFunction(cx, &argv[-2], 0);
        if (fun) {
            JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL,
                                 JSMSG_INCOMPATIBLE_PROTO,
                                 clasp->name, JS_GetFunctionName(fun),
                                 OBJ_GET_CLASS(cx, obj)->name);
        }
    }
    return JS_FALSE;
}

JS_PUBLIC_API(JSBool)
JS_HasInstance(JSContext *cx, JSObject *obj, jsval v, JSBool *bp)
{
    return js_HasInstance(cx, obj, v, bp);
}

JS_PUBLIC_API(void *)
JS_GetPrivate(JSContext *cx, JSObject *obj)
{
    jsval v;

    JS_ASSERT(OBJ_GET_CLASS(cx, obj)->flags & JSCLASS_HAS_PRIVATE);
    v = GC_AWARE_GET_SLOT(cx, obj, JSSLOT_PRIVATE);
    if (!JSVAL_IS_INT(v))
        return NULL;
    return JSVAL_TO_PRIVATE(v);
}

JS_PUBLIC_API(JSBool)
JS_SetPrivate(JSContext *cx, JSObject *obj, void *data)
{
    JS_ASSERT(OBJ_GET_CLASS(cx, obj)->flags & JSCLASS_HAS_PRIVATE);
    OBJ_SET_SLOT(cx, obj, JSSLOT_PRIVATE, PRIVATE_TO_JSVAL(data));
    return JS_TRUE;
}

JS_PUBLIC_API(void *)
JS_GetInstancePrivate(JSContext *cx, JSObject *obj, JSClass *clasp,
                      jsval *argv)
{
    if (!JS_InstanceOf(cx, obj, clasp, argv))
        return NULL;
    return JS_GetPrivate(cx, obj);
}

JS_PUBLIC_API(JSObject *)
JS_GetPrototype(JSContext *cx, JSObject *obj)
{
    JSObject *proto;

    CHECK_REQUEST(cx);
    proto = JSVAL_TO_OBJECT(GC_AWARE_GET_SLOT(cx, obj, JSSLOT_PROTO));

    /* Beware ref to dead object (we may be called from obj's finalizer). */
    return proto && proto->map ? proto : NULL;
}

JS_PUBLIC_API(JSBool)
JS_SetPrototype(JSContext *cx, JSObject *obj, JSObject *proto)
{
    CHECK_REQUEST(cx);
    if (obj->map->ops->setProto)
        return obj->map->ops->setProto(cx, obj, JSSLOT_PROTO, proto);
    OBJ_SET_SLOT(cx, obj, JSSLOT_PROTO, OBJECT_TO_JSVAL(proto));
    return JS_TRUE;
}

JS_PUBLIC_API(JSObject *)
JS_GetParent(JSContext *cx, JSObject *obj)
{
    JSObject *parent;

    parent = JSVAL_TO_OBJECT(GC_AWARE_GET_SLOT(cx, obj, JSSLOT_PARENT));

    /* Beware ref to dead object (we may be called from obj's finalizer). */
    return parent && parent->map ? parent : NULL;
}

JS_PUBLIC_API(JSBool)
JS_SetParent(JSContext *cx, JSObject *obj, JSObject *parent)
{
    CHECK_REQUEST(cx);
    if (obj->map->ops->setParent)
        return obj->map->ops->setParent(cx, obj, JSSLOT_PARENT, parent);
    OBJ_SET_SLOT(cx, obj, JSSLOT_PARENT, OBJECT_TO_JSVAL(parent));
    return JS_TRUE;
}

JS_PUBLIC_API(JSObject *)
JS_GetConstructor(JSContext *cx, JSObject *proto)
{
    jsval cval;

    CHECK_REQUEST(cx);
    if (!OBJ_GET_PROPERTY(cx, proto,
                          ATOM_TO_JSID(cx->runtime->atomState.constructorAtom),
                          &cval)) {
        return NULL;
    }
    if (!VALUE_IS_FUNCTION(cx, cval)) {
        JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_NO_CONSTRUCTOR,
                             OBJ_GET_CLASS(cx, proto)->name);
        return NULL;
    }
    return JSVAL_TO_OBJECT(cval);
}

JS_PUBLIC_API(JSBool)
JS_GetObjectId(JSContext *cx, JSObject *obj, jsid *idp)
{
    JS_ASSERT(((jsid)obj & JSID_TAGMASK) == 0);
    *idp = OBJECT_TO_JSID(obj);
    return JS_TRUE;
}

JS_PUBLIC_API(JSObject *)
JS_NewObject(JSContext *cx, JSClass *clasp, JSObject *proto, JSObject *parent)
{
    CHECK_REQUEST(cx);
    if (!clasp)
        clasp = &js_ObjectClass;    /* default class is Object */
    return js_NewObject(cx, clasp, proto, parent);
}

JS_PUBLIC_API(JSBool)
JS_SealObject(JSContext *cx, JSObject *obj, JSBool deep)
{
    JSScope *scope;
    JSIdArray *ida;
    uint32 nslots;
    jsval v, *vp, *end;

    if (!OBJ_IS_NATIVE(obj)) {
        JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL,
                             JSMSG_CANT_SEAL_OBJECT,
                             OBJ_GET_CLASS(cx, obj)->name);
        return JS_FALSE;
    }

    scope = OBJ_SCOPE(obj);

#if defined JS_THREADSAFE && defined DEBUG
    /* Insist on scope being used exclusively by cx's thread. */
    if (scope->ownercx != cx) {
        JS_LOCK_OBJ(cx, obj);
        JS_ASSERT(OBJ_SCOPE(obj) == scope);
        JS_ASSERT(scope->ownercx == cx);
        JS_UNLOCK_SCOPE(cx, scope);
    }
#endif

    /* Nothing to do if obj's scope is already sealed. */
    if (SCOPE_IS_SEALED(scope))
        return JS_TRUE;

    /* XXX Enumerate lazy properties now, as they can't be added later. */
    ida = JS_Enumerate(cx, obj);
    if (!ida)
        return JS_FALSE;
    JS_DestroyIdArray(cx, ida);

    /* Ensure that obj has its own, mutable scope, and seal that scope. */
    JS_LOCK_OBJ(cx, obj);
    scope = js_GetMutableScope(cx, obj);
    if (scope)
        SCOPE_SET_SEALED(scope);
    JS_UNLOCK_OBJ(cx, obj);
    if (!scope)
        return JS_FALSE;

    /* If we are not sealing an entire object graph, we're done. */
    if (!deep)
        return JS_TRUE;

    /* Walk obj->slots and if any value is a non-null object, seal it. */
    nslots = JS_MIN(scope->map.freeslot, scope->map.nslots);
    for (vp = obj->slots, end = vp + nslots; vp < end; vp++) {
        v = *vp;
        if (JSVAL_IS_PRIMITIVE(v))
            continue;
        if (!JS_SealObject(cx, JSVAL_TO_OBJECT(v), deep))
            return JS_FALSE;
    }
    return JS_TRUE;
}

JS_PUBLIC_API(JSObject *)
JS_ConstructObject(JSContext *cx, JSClass *clasp, JSObject *proto,
                   JSObject *parent)
{
    CHECK_REQUEST(cx);
    if (!clasp)
        clasp = &js_ObjectClass;    /* default class is Object */
    return js_ConstructObject(cx, clasp, proto, parent, 0, NULL);
}

JS_PUBLIC_API(JSObject *)
JS_ConstructObjectWithArguments(JSContext *cx, JSClass *clasp, JSObject *proto,
                                JSObject *parent, uintN argc, jsval *argv)
{
    CHECK_REQUEST(cx);
    if (!clasp)
        clasp = &js_ObjectClass;    /* default class is Object */
    return js_ConstructObject(cx, clasp, proto, parent, argc, argv);
}

static JSBool
DefineProperty(JSContext *cx, JSObject *obj, const char *name, jsval value,
               JSPropertyOp getter, JSPropertyOp setter, uintN attrs,
               uintN flags, intN tinyid)
{
    jsid id;
    JSAtom *atom;

    if (attrs & JSPROP_INDEX) {
        id = INT_TO_JSID(JS_PTR_TO_INT32(name));
        atom = NULL;
        attrs &= ~JSPROP_INDEX;
    } else {
        atom = js_Atomize(cx, name, strlen(name), 0);
        if (!atom)
            return JS_FALSE;
        id = ATOM_TO_JSID(atom);
    }
    if (flags != 0 && OBJ_IS_NATIVE(obj)) {
        return js_DefineNativeProperty(cx, obj, id, value, getter, setter,
                                       attrs, flags, tinyid, NULL);
    }
    return OBJ_DEFINE_PROPERTY(cx, obj, id, value, getter, setter, attrs,
                               NULL);
}

#define AUTO_NAMELEN(s,n)   (((n) == (size_t)-1) ? js_strlen(s) : (n))

static JSBool
DefineUCProperty(JSContext *cx, JSObject *obj,
                 const jschar *name, size_t namelen, jsval value,
                 JSPropertyOp getter, JSPropertyOp setter, uintN attrs,
                 uintN flags, intN tinyid)
{
    JSAtom *atom;

    atom = js_AtomizeChars(cx, name, AUTO_NAMELEN(name, namelen), 0);
    if (!atom)
        return JS_FALSE;
    if (flags != 0 && OBJ_IS_NATIVE(obj)) {
        return js_DefineNativeProperty(cx, obj, ATOM_TO_JSID(atom), value,
                                       getter, setter, attrs, flags, tinyid,
                                       NULL);
    }
    return OBJ_DEFINE_PROPERTY(cx, obj, ATOM_TO_JSID(atom), value,
                               getter, setter, attrs, NULL);
}

JS_PUBLIC_API(JSObject *)
JS_DefineObject(JSContext *cx, JSObject *obj, const char *name, JSClass *clasp,
                JSObject *proto, uintN attrs)
{
    JSObject *nobj;

    CHECK_REQUEST(cx);
    if (!clasp)
        clasp = &js_ObjectClass;    /* default class is Object */
    nobj = js_NewObject(cx, clasp, proto, obj);
    if (!nobj)
        return NULL;
    if (!DefineProperty(cx, obj, name, OBJECT_TO_JSVAL(nobj), NULL, NULL, attrs,
                        0, 0)) {
        cx->weakRoots.newborn[GCX_OBJECT] = NULL;
        return NULL;
    }
    return nobj;
}

JS_PUBLIC_API(JSBool)
JS_DefineConstDoubles(JSContext *cx, JSObject *obj, JSConstDoubleSpec *cds)
{
    JSBool ok;
    jsval value;
    uintN flags;

    CHECK_REQUEST(cx);
    for (ok = JS_TRUE; cds->name; cds++) {
        ok = js_NewNumberValue(cx, cds->dval, &value);
        if (!ok)
            break;
        flags = cds->flags;
        if (!flags)
            flags = JSPROP_READONLY | JSPROP_PERMANENT;
        ok = DefineProperty(cx, obj, cds->name, value, NULL, NULL, flags, 0, 0);
        if (!ok)
            break;
    }
    return ok;
}

JS_PUBLIC_API(JSBool)
JS_DefineProperties(JSContext *cx, JSObject *obj, JSPropertySpec *ps)
{
    JSBool ok;

    CHECK_REQUEST(cx);
    for (ok = JS_TRUE; ps->name; ps++) {
        ok = DefineProperty(cx, obj, ps->name, JSVAL_VOID,
                            ps->getter, ps->setter, ps->flags,
                            SPROP_HAS_SHORTID, ps->tinyid);
        if (!ok)
            break;
    }
    return ok;
}

JS_PUBLIC_API(JSBool)
JS_DefineProperty(JSContext *cx, JSObject *obj, const char *name, jsval value,
                  JSPropertyOp getter, JSPropertyOp setter, uintN attrs)
{
    CHECK_REQUEST(cx);
    return DefineProperty(cx, obj, name, value, getter, setter, attrs, 0, 0);
}

JS_PUBLIC_API(JSBool)
JS_DefinePropertyWithTinyId(JSContext *cx, JSObject *obj, const char *name,
                            int8 tinyid, jsval value,
                            JSPropertyOp getter, JSPropertyOp setter,
                            uintN attrs)
{
    CHECK_REQUEST(cx);
    return DefineProperty(cx, obj, name, value, getter, setter, attrs,
                          SPROP_HAS_SHORTID, tinyid);
}

static JSBool
LookupProperty(JSContext *cx, JSObject *obj, const char *name, JSObject **objp,
               JSProperty **propp)
{
    JSAtom *atom;

    atom = js_Atomize(cx, name, strlen(name), 0);
    if (!atom)
        return JS_FALSE;
    return OBJ_LOOKUP_PROPERTY(cx, obj, ATOM_TO_JSID(atom), objp, propp);
}

static JSBool
LookupUCProperty(JSContext *cx, JSObject *obj,
                 const jschar *name, size_t namelen,
                 JSObject **objp, JSProperty **propp)
{
    JSAtom *atom;

    atom = js_AtomizeChars(cx, name, AUTO_NAMELEN(name, namelen), 0);
    if (!atom)
        return JS_FALSE;
    return OBJ_LOOKUP_PROPERTY(cx, obj, ATOM_TO_JSID(atom), objp, propp);
}

JS_PUBLIC_API(JSBool)
JS_AliasProperty(JSContext *cx, JSObject *obj, const char *name,
                 const char *alias)
{
    JSObject *obj2;
    JSProperty *prop;
    JSAtom *atom;
    JSBool ok;
    JSScopeProperty *sprop;

    CHECK_REQUEST(cx);
    if (!LookupProperty(cx, obj, name, &obj2, &prop))
        return JS_FALSE;
    if (!prop) {
        js_ReportIsNotDefined(cx, name);
        return JS_FALSE;
    }
    if (obj2 != obj || !OBJ_IS_NATIVE(obj)) {
        OBJ_DROP_PROPERTY(cx, obj2, prop);
        JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_CANT_ALIAS,
                             alias, name, OBJ_GET_CLASS(cx, obj2)->name);
        return JS_FALSE;
    }
    atom = js_Atomize(cx, alias, strlen(alias), 0);
    if (!atom) {
        ok = JS_FALSE;
    } else {
        sprop = (JSScopeProperty *)prop;
        ok = (js_AddNativeProperty(cx, obj, ATOM_TO_JSID(atom),
                                   sprop->getter, sprop->setter, sprop->slot,
                                   sprop->attrs, sprop->flags | SPROP_IS_ALIAS,
                                   sprop->shortid)
              != NULL);
    }
    OBJ_DROP_PROPERTY(cx, obj, prop);
    return ok;
}

static jsval
LookupResult(JSContext *cx, JSObject *obj, JSObject *obj2, JSProperty *prop)
{
    JSScopeProperty *sprop;
    jsval rval;

    if (!prop) {
        /* XXX bad API: no way to tell "not defined" from "void value" */
        return JSVAL_VOID;
    }
    if (OBJ_IS_NATIVE(obj2)) {
        /* Peek at the native property's slot value, without doing a Get. */
        sprop = (JSScopeProperty *)prop;
        rval = SPROP_HAS_VALID_SLOT(sprop, OBJ_SCOPE(obj2))
               ? LOCKED_OBJ_GET_SLOT(obj2, sprop->slot)
               : JSVAL_TRUE;
    } else {
        /* XXX bad API: no way to return "defined but value unknown" */
        rval = JSVAL_TRUE;
    }
    OBJ_DROP_PROPERTY(cx, obj2, prop);
    return rval;
}

static JSBool
GetPropertyAttributes(JSContext *cx, JSObject *obj, JSAtom *atom,
                      uintN *attrsp, JSBool *foundp,
                      JSPropertyOp *getterp, JSPropertyOp *setterp)
{
    JSObject *obj2;
    JSProperty *prop;
    JSBool ok;

    if (!atom)
        return JS_FALSE;
    if (!OBJ_LOOKUP_PROPERTY(cx, obj, ATOM_TO_JSID(atom), &obj2, &prop))
        return JS_FALSE;

    if (!prop || obj != obj2) {
        *attrsp = 0;
        *foundp = JS_FALSE;
        if (getterp)
            *getterp = NULL;
        if (setterp)
            *setterp = NULL;
        if (prop)
            OBJ_DROP_PROPERTY(cx, obj2, prop);
        return JS_TRUE;
    }

    *foundp = JS_TRUE;
    ok = OBJ_GET_ATTRIBUTES(cx, obj, ATOM_TO_JSID(atom), prop, attrsp);
    if (ok && OBJ_IS_NATIVE(obj)) {
        JSScopeProperty *sprop = (JSScopeProperty *) prop;

        if (getterp)
            *getterp = sprop->getter;
        if (setterp)
            *setterp = sprop->setter;
    }
    OBJ_DROP_PROPERTY(cx, obj, prop);
    return ok;
}

static JSBool
SetPropertyAttributes(JSContext *cx, JSObject *obj, JSAtom *atom,
                      uintN attrs, JSBool *foundp)
{
    JSObject *obj2;
    JSProperty *prop;
    JSBool ok;

    if (!atom)
        return JS_FALSE;
    if (!OBJ_LOOKUP_PROPERTY(cx, obj, ATOM_TO_JSID(atom), &obj2, &prop))
        return JS_FALSE;
    if (!prop || obj != obj2) {
        *foundp = JS_FALSE;
        if (prop)
            OBJ_DROP_PROPERTY(cx, obj2, prop);
        return JS_TRUE;
    }

    *foundp = JS_TRUE;
    ok = OBJ_SET_ATTRIBUTES(cx, obj, ATOM_TO_JSID(atom), prop, &attrs);
    OBJ_DROP_PROPERTY(cx, obj, prop);
    return ok;
}

JS_PUBLIC_API(JSBool)
JS_GetPropertyAttributes(JSContext *cx, JSObject *obj, const char *name,
                         uintN *attrsp, JSBool *foundp)
{
    CHECK_REQUEST(cx);
    return GetPropertyAttributes(cx, obj,
                                 js_Atomize(cx, name, strlen(name), 0),
                                 attrsp, foundp, NULL, NULL);
}

JS_PUBLIC_API(JSBool)
JS_GetPropertyAttrsGetterAndSetter(JSContext *cx, JSObject *obj,
                                   const char *name,
                                   uintN *attrsp, JSBool *foundp,
                                   JSPropertyOp *getterp,
                                   JSPropertyOp *setterp)
{
    CHECK_REQUEST(cx);
    return GetPropertyAttributes(cx, obj,
                                 js_Atomize(cx, name, strlen(name), 0),
                                 attrsp, foundp, getterp, setterp);
}

JS_PUBLIC_API(JSBool)
JS_SetPropertyAttributes(JSContext *cx, JSObject *obj, const char *name,
                         uintN attrs, JSBool *foundp)
{
    CHECK_REQUEST(cx);
    return SetPropertyAttributes(cx, obj,
                                 js_Atomize(cx, name, strlen(name), 0),
                                 attrs, foundp);
}

JS_PUBLIC_API(JSBool)
JS_HasProperty(JSContext *cx, JSObject *obj, const char *name, JSBool *foundp)
{
    JSBool ok;
    JSObject *obj2;
    JSProperty *prop;

    CHECK_REQUEST(cx);
    ok = LookupProperty(cx, obj, name, &obj2, &prop);
    if (ok) {
        *foundp = (prop != NULL);
        if (prop)
            OBJ_DROP_PROPERTY(cx, obj2, prop);
    }
    return ok;
}

JS_PUBLIC_API(JSBool)
JS_LookupProperty(JSContext *cx, JSObject *obj, const char *name, jsval *vp)
{
    JSBool ok;
    JSObject *obj2;
    JSProperty *prop;

    CHECK_REQUEST(cx);
    ok = LookupProperty(cx, obj, name, &obj2, &prop);
    if (ok)
        *vp = LookupResult(cx, obj, obj2, prop);
    return ok;
}

JS_PUBLIC_API(JSBool)
JS_LookupPropertyWithFlags(JSContext *cx, JSObject *obj, const char *name,
                           uintN flags, jsval *vp)
{
    JSAtom *atom;
    JSBool ok;
    JSObject *obj2;
    JSProperty *prop;

    CHECK_REQUEST(cx);
    atom = js_Atomize(cx, name, strlen(name), 0);
    if (!atom)
        return JS_FALSE;
    ok = OBJ_IS_NATIVE(obj)
         ? js_LookupPropertyWithFlags(cx, obj, ATOM_TO_JSID(atom), flags,
                                      &obj2, &prop)
         : OBJ_LOOKUP_PROPERTY(cx, obj, ATOM_TO_JSID(atom), &obj2, &prop);
    if (ok)
        *vp = LookupResult(cx, obj, obj2, prop);
    return ok;
}

JS_PUBLIC_API(JSBool)
JS_GetProperty(JSContext *cx, JSObject *obj, const char *name, jsval *vp)
{
    JSAtom *atom;

    CHECK_REQUEST(cx);
    atom = js_Atomize(cx, name, strlen(name), 0);
    if (!atom)
        return JS_FALSE;
    return OBJ_GET_PROPERTY(cx, obj, ATOM_TO_JSID(atom), vp);
}

JS_PUBLIC_API(JSBool)
JS_GetMethodById(JSContext *cx, JSObject *obj, jsid id, JSObject **objp,
                 jsval *vp)
{
    CHECK_REQUEST(cx);

#if JS_HAS_XML_SUPPORT
    if (OBJECT_IS_XML(cx, obj)) {
        JSXMLObjectOps *ops;

        ops = (JSXMLObjectOps *) obj->map->ops;
        obj = ops->getMethod(cx, obj, id, vp);
        if (!obj)
            return JS_FALSE;
    } else
#endif
    {
        if (!OBJ_GET_PROPERTY(cx, obj, id, vp))
            return JS_FALSE;
    }

    *objp = obj;
    return JS_TRUE;
}

JS_PUBLIC_API(JSBool)
JS_GetMethod(JSContext *cx, JSObject *obj, const char *name, JSObject **objp,
             jsval *vp)
{
    JSAtom *atom;

    atom = js_Atomize(cx, name, strlen(name), 0);
    if (!atom)
        return JS_FALSE;
    return JS_GetMethodById(cx, obj, ATOM_TO_JSID(atom), objp, vp);
}

JS_PUBLIC_API(JSBool)
JS_SetProperty(JSContext *cx, JSObject *obj, const char *name, jsval *vp)
{
    JSAtom *atom;

    CHECK_REQUEST(cx);
    atom = js_Atomize(cx, name, strlen(name), 0);
    if (!atom)
        return JS_FALSE;
    return OBJ_SET_PROPERTY(cx, obj, ATOM_TO_JSID(atom), vp);
}

JS_PUBLIC_API(JSBool)
JS_DeleteProperty(JSContext *cx, JSObject *obj, const char *name)
{
    jsval junk;

    CHECK_REQUEST(cx);
    return JS_DeleteProperty2(cx, obj, name, &junk);
}

JS_PUBLIC_API(JSBool)
JS_DeleteProperty2(JSContext *cx, JSObject *obj, const char *name,
                   jsval *rval)
{
    JSAtom *atom;

    CHECK_REQUEST(cx);
    atom = js_Atomize(cx, name, strlen(name), 0);
    if (!atom)
        return JS_FALSE;
    return OBJ_DELETE_PROPERTY(cx, obj, ATOM_TO_JSID(atom), rval);
}

JS_PUBLIC_API(JSBool)
JS_DefineUCProperty(JSContext *cx, JSObject *obj,
                    const jschar *name, size_t namelen, jsval value,
                    JSPropertyOp getter, JSPropertyOp setter,
                    uintN attrs)
{
    CHECK_REQUEST(cx);
    return DefineUCProperty(cx, obj, name, namelen, value, getter, setter,
                            attrs, 0, 0);
}

JS_PUBLIC_API(JSBool)
JS_GetUCPropertyAttributes(JSContext *cx, JSObject *obj,
                           const jschar *name, size_t namelen,
                           uintN *attrsp, JSBool *foundp)
{
    CHECK_REQUEST(cx);
    return GetPropertyAttributes(cx, obj,
                    js_AtomizeChars(cx, name, AUTO_NAMELEN(name, namelen), 0),
                    attrsp, foundp, NULL, NULL);
}

JS_PUBLIC_API(JSBool)
JS_GetUCPropertyAttrsGetterAndSetter(JSContext *cx, JSObject *obj,
                                     const jschar *name, size_t namelen,
                                     uintN *attrsp, JSBool *foundp,
                                     JSPropertyOp *getterp,
                                     JSPropertyOp *setterp)
{
    CHECK_REQUEST(cx);
    return GetPropertyAttributes(cx, obj,
                    js_AtomizeChars(cx, name, AUTO_NAMELEN(name, namelen), 0),
                    attrsp, foundp, getterp, setterp);
}

JS_PUBLIC_API(JSBool)
JS_SetUCPropertyAttributes(JSContext *cx, JSObject *obj,
                           const jschar *name, size_t namelen,
                           uintN attrs, JSBool *foundp)
{
    CHECK_REQUEST(cx);
    return SetPropertyAttributes(cx, obj,
                    js_AtomizeChars(cx, name, AUTO_NAMELEN(name, namelen), 0),
                    attrs, foundp);
}

JS_PUBLIC_API(JSBool)
JS_DefineUCPropertyWithTinyId(JSContext *cx, JSObject *obj,
                              const jschar *name, size_t namelen,
                              int8 tinyid, jsval value,
                              JSPropertyOp getter, JSPropertyOp setter,
                              uintN attrs)
{
    CHECK_REQUEST(cx);
    return DefineUCProperty(cx, obj, name, namelen, value, getter, setter,
                            attrs, SPROP_HAS_SHORTID, tinyid);
}

JS_PUBLIC_API(JSBool)
JS_HasUCProperty(JSContext *cx, JSObject *obj,
                 const jschar *name, size_t namelen,
                 JSBool *vp)
{
    JSBool ok;
    JSObject *obj2;
    JSProperty *prop;

    CHECK_REQUEST(cx);
    ok = LookupUCProperty(cx, obj, name, namelen, &obj2, &prop);
    if (ok) {
        *vp = (prop != NULL);
        if (prop)
            OBJ_DROP_PROPERTY(cx, obj2, prop);
    }
    return ok;
}

JS_PUBLIC_API(JSBool)
JS_LookupUCProperty(JSContext *cx, JSObject *obj,
                    const jschar *name, size_t namelen,
                    jsval *vp)
{
    JSBool ok;
    JSObject *obj2;
    JSProperty *prop;

    CHECK_REQUEST(cx);
    ok = LookupUCProperty(cx, obj, name, namelen, &obj2, &prop);
    if (ok)
        *vp = LookupResult(cx, obj, obj2, prop);
    return ok;
}

JS_PUBLIC_API(JSBool)
JS_GetUCProperty(JSContext *cx, JSObject *obj,
                 const jschar *name, size_t namelen,
                 jsval *vp)
{
    JSAtom *atom;

    CHECK_REQUEST(cx);
    atom = js_AtomizeChars(cx, name, AUTO_NAMELEN(name, namelen), 0);
    if (!atom)
        return JS_FALSE;
    return OBJ_GET_PROPERTY(cx, obj, ATOM_TO_JSID(atom), vp);
}

JS_PUBLIC_API(JSBool)
JS_SetUCProperty(JSContext *cx, JSObject *obj,
                 const jschar *name, size_t namelen,
                 jsval *vp)
{
    JSAtom *atom;

    CHECK_REQUEST(cx);
    atom = js_AtomizeChars(cx, name, AUTO_NAMELEN(name, namelen), 0);
    if (!atom)
        return JS_FALSE;
    return OBJ_SET_PROPERTY(cx, obj, ATOM_TO_JSID(atom), vp);
}

JS_PUBLIC_API(JSBool)
JS_DeleteUCProperty2(JSContext *cx, JSObject *obj,
                     const jschar *name, size_t namelen,
                     jsval *rval)
{
    JSAtom *atom;

    CHECK_REQUEST(cx);
    atom = js_AtomizeChars(cx, name, AUTO_NAMELEN(name, namelen), 0);
    if (!atom)
        return JS_FALSE;
    return OBJ_DELETE_PROPERTY(cx, obj, ATOM_TO_JSID(atom), rval);
}

JS_PUBLIC_API(JSObject *)
JS_NewArrayObject(JSContext *cx, jsint length, jsval *vector)
{
    CHECK_REQUEST(cx);
    /* NB: jsuint cast does ToUint32. */
    return js_NewArrayObject(cx, (jsuint)length, vector);
}

JS_PUBLIC_API(JSBool)
JS_IsArrayObject(JSContext *cx, JSObject *obj)
{
    return OBJ_GET_CLASS(cx, obj) == &js_ArrayClass;
}

JS_PUBLIC_API(JSBool)
JS_GetArrayLength(JSContext *cx, JSObject *obj, jsuint *lengthp)
{
    CHECK_REQUEST(cx);
    return js_GetLengthProperty(cx, obj, lengthp);
}

JS_PUBLIC_API(JSBool)
JS_SetArrayLength(JSContext *cx, JSObject *obj, jsuint length)
{
    CHECK_REQUEST(cx);
    return js_SetLengthProperty(cx, obj, length);
}

JS_PUBLIC_API(JSBool)
JS_HasArrayLength(JSContext *cx, JSObject *obj, jsuint *lengthp)
{
    CHECK_REQUEST(cx);
    return js_HasLengthProperty(cx, obj, lengthp);
}

JS_PUBLIC_API(JSBool)
JS_DefineElement(JSContext *cx, JSObject *obj, jsint index, jsval value,
                 JSPropertyOp getter, JSPropertyOp setter, uintN attrs)
{
    CHECK_REQUEST(cx);
    return OBJ_DEFINE_PROPERTY(cx, obj, INT_TO_JSID(index), value,
                               getter, setter, attrs, NULL);
}

JS_PUBLIC_API(JSBool)
JS_AliasElement(JSContext *cx, JSObject *obj, const char *name, jsint alias)
{
    JSObject *obj2;
    JSProperty *prop;
    JSScopeProperty *sprop;
    JSBool ok;

    CHECK_REQUEST(cx);
    if (!LookupProperty(cx, obj, name, &obj2, &prop))
        return JS_FALSE;
    if (!prop) {
        js_ReportIsNotDefined(cx, name);
        return JS_FALSE;
    }
    if (obj2 != obj || !OBJ_IS_NATIVE(obj)) {
        char numBuf[12];
        OBJ_DROP_PROPERTY(cx, obj2, prop);
        JS_snprintf(numBuf, sizeof numBuf, "%ld", (long)alias);
        JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_CANT_ALIAS,
                             numBuf, name, OBJ_GET_CLASS(cx, obj2)->name);
        return JS_FALSE;
    }
    sprop = (JSScopeProperty *)prop;
    ok = (js_AddNativeProperty(cx, obj, INT_TO_JSID(alias),
                               sprop->getter, sprop->setter, sprop->slot,
                               sprop->attrs, sprop->flags | SPROP_IS_ALIAS,
                               sprop->shortid)
          != NULL);
    OBJ_DROP_PROPERTY(cx, obj, prop);
    return ok;
}

JS_PUBLIC_API(JSBool)
JS_HasElement(JSContext *cx, JSObject *obj, jsint index, JSBool *foundp)
{
    JSBool ok;
    JSObject *obj2;
    JSProperty *prop;

    CHECK_REQUEST(cx);
    ok = OBJ_LOOKUP_PROPERTY(cx, obj, INT_TO_JSID(index), &obj2, &prop);
    if (ok) {
        *foundp = (prop != NULL);
        if (prop)
            OBJ_DROP_PROPERTY(cx, obj2, prop);
    }
    return ok;
}

JS_PUBLIC_API(JSBool)
JS_LookupElement(JSContext *cx, JSObject *obj, jsint index, jsval *vp)
{
    JSBool ok;
    JSObject *obj2;
    JSProperty *prop;

    CHECK_REQUEST(cx);
    ok = OBJ_LOOKUP_PROPERTY(cx, obj, INT_TO_JSID(index), &obj2, &prop);
    if (ok)
        *vp = LookupResult(cx, obj, obj2, prop);
    return ok;
}

JS_PUBLIC_API(JSBool)
JS_GetElement(JSContext *cx, JSObject *obj, jsint index, jsval *vp)
{
    CHECK_REQUEST(cx);
    return OBJ_GET_PROPERTY(cx, obj, INT_TO_JSID(index), vp);
}

JS_PUBLIC_API(JSBool)
JS_SetElement(JSContext *cx, JSObject *obj, jsint index, jsval *vp)
{
    CHECK_REQUEST(cx);
    return OBJ_SET_PROPERTY(cx, obj, INT_TO_JSID(index), vp);
}

JS_PUBLIC_API(JSBool)
JS_DeleteElement(JSContext *cx, JSObject *obj, jsint index)
{
    jsval junk;

    CHECK_REQUEST(cx);
    return JS_DeleteElement2(cx, obj, index, &junk);
}

JS_PUBLIC_API(JSBool)
JS_DeleteElement2(JSContext *cx, JSObject *obj, jsint index, jsval *rval)
{
    CHECK_REQUEST(cx);
    return OBJ_DELETE_PROPERTY(cx, obj, INT_TO_JSID(index), rval);
}

JS_PUBLIC_API(void)
JS_ClearScope(JSContext *cx, JSObject *obj)
{
    CHECK_REQUEST(cx);

    if (obj->map->ops->clear)
        obj->map->ops->clear(cx, obj);

    /* Clear cached class objects on the global object. */
    if (JS_GET_CLASS(cx, obj)->flags & JSCLASS_IS_GLOBAL) {
        JSProtoKey key;

        for (key = JSProto_Null; key < JSProto_LIMIT; key++)
            JS_SetReservedSlot(cx, obj, key, JSVAL_VOID);
    }
}

JS_PUBLIC_API(JSIdArray *)
JS_Enumerate(JSContext *cx, JSObject *obj)
{
    jsint i, n;
    jsval iter_state, num_properties;
    jsid id;
    JSIdArray *ida;
    jsval *vector;

    CHECK_REQUEST(cx);

    ida = NULL;
    iter_state = JSVAL_NULL;

    /* Get the number of properties to enumerate. */
    if (!OBJ_ENUMERATE(cx, obj, JSENUMERATE_INIT, &iter_state, &num_properties))
        goto error;
    if (!JSVAL_IS_INT(num_properties)) {
        JS_ASSERT(0);
        goto error;
    }

    /* Grow as needed if we don't know the exact amount ahead of time. */
    n = JSVAL_TO_INT(num_properties);
    if (n <= 0)
        n = 8;

    /* Create an array of jsids large enough to hold all the properties */
    ida = js_NewIdArray(cx, n);
    if (!ida)
        goto error;

    i = 0;
    vector = &ida->vector[0];
    for (;;) {
        if (!OBJ_ENUMERATE(cx, obj, JSENUMERATE_NEXT, &iter_state, &id))
            goto error;

        /* No more jsid's to enumerate ? */
        if (iter_state == JSVAL_NULL)
            break;

        if (i == ida->length) {
            ida = js_SetIdArrayLength(cx, ida, ida->length * 2);
            if (!ida)
                goto error;
            vector = &ida->vector[0];
        }
        vector[i++] = id;
    }
    return js_SetIdArrayLength(cx, ida, i);

error:
    if (iter_state != JSVAL_NULL)
        OBJ_ENUMERATE(cx, obj, JSENUMERATE_DESTROY, &iter_state, 0);
    if (ida)
        JS_DestroyIdArray(cx, ida);
    return NULL;
}

/*
 * XXX reverse iterator for properties, unreverse and meld with jsinterp.c's
 *     prop_iterator_class somehow...
 * + preserve the OBJ_ENUMERATE API while optimizing the native object case
 * + native case here uses a JSScopeProperty *, but that iterates in reverse!
 * + so we make non-native match, by reverse-iterating after JS_Enumerating
 */
#define JSSLOT_ITER_INDEX       (JSSLOT_PRIVATE + 1)

#if JSSLOT_ITER_INDEX >= JS_INITIAL_NSLOTS
# error "JSSLOT_ITER_INDEX botch!"
#endif

static void
prop_iter_finalize(JSContext *cx, JSObject *obj)
{
    jsval v;
    jsint i;
    JSIdArray *ida;

    v = GC_AWARE_GET_SLOT(cx, obj, JSSLOT_ITER_INDEX);
    if (JSVAL_IS_VOID(v))
        return;

    i = JSVAL_TO_INT(v);
    if (i >= 0) {
        /* Non-native case: destroy the ida enumerated when obj was created. */
        ida = (JSIdArray *) JS_GetPrivate(cx, obj);
        if (ida)
            JS_DestroyIdArray(cx, ida);
    }
}

static uint32
prop_iter_mark(JSContext *cx, JSObject *obj, void *arg)
{
    jsval v;
    jsint i, n;
    JSScopeProperty *sprop;
    JSIdArray *ida;
    jsid id;

    v = GC_AWARE_GET_SLOT(cx, obj, JSSLOT_PRIVATE);
    JS_ASSERT(!JSVAL_IS_VOID(v));

    i = JSVAL_TO_INT(OBJ_GET_SLOT(cx, obj, JSSLOT_ITER_INDEX));
    if (i < 0) {
        /* Native case: just mark the next property to visit. */
        sprop = (JSScopeProperty *) JSVAL_TO_PRIVATE(v);
        if (sprop)
            MARK_SCOPE_PROPERTY(cx, sprop);
    } else {
        /* Non-native case: mark each id in the JSIdArray private. */
        ida = (JSIdArray *) JSVAL_TO_PRIVATE(v);
        for (i = 0, n = ida->length; i < n; i++) {
            id = ida->vector[i];
            MARK_ID(cx, id);
        }
    }
    return 0;
}

static JSClass prop_iter_class = {
    "PropertyIterator",
    JSCLASS_HAS_PRIVATE | JSCLASS_HAS_RESERVED_SLOTS(1),
    JS_PropertyStub,  JS_PropertyStub, JS_PropertyStub, JS_PropertyStub,
    JS_EnumerateStub, JS_ResolveStub,  JS_ConvertStub,  prop_iter_finalize,
    NULL,             NULL,            NULL,            NULL,
    NULL,             NULL,            prop_iter_mark,  NULL
};

JS_PUBLIC_API(JSObject *)
JS_NewPropertyIterator(JSContext *cx, JSObject *obj)
{
    JSObject *iterobj;
    JSScope *scope;
    void *pdata;
    jsint index;
    JSIdArray *ida;

    CHECK_REQUEST(cx);
    iterobj = js_NewObject(cx, &prop_iter_class, NULL, obj);
    if (!iterobj)
        return NULL;

    if (OBJ_IS_NATIVE(obj)) {
        /* Native case: start with the last property in obj's own scope. */
        scope = OBJ_SCOPE(obj);
        pdata = (scope->object == obj) ? scope->lastProp : NULL;
        index = -1;
    } else {
        JSTempValueRooter tvr;

        /*
         * Non-native case: enumerate a JSIdArray and keep it via private.
         *
         * Note: we have to make sure that we root obj around the call to
         * JS_Enumerate to protect against multiple allocations under it.
         */
        JS_PUSH_SINGLE_TEMP_ROOT(cx, OBJECT_TO_JSVAL(iterobj), &tvr);
        ida = JS_Enumerate(cx, obj);
        JS_POP_TEMP_ROOT(cx, &tvr);
        if (!ida)
            goto bad;
        pdata = ida;
        index = ida->length;
    }

    /* iterobj can not escape to other threads here. */
    iterobj->slots[JSSLOT_PRIVATE] = PRIVATE_TO_JSVAL(pdata);
    iterobj->slots[JSSLOT_ITER_INDEX] = INT_TO_JSVAL(index);
    return iterobj;

  bad:
    cx->weakRoots.newborn[GCX_OBJECT] = NULL;
    return NULL;
}

JS_PUBLIC_API(JSBool)
JS_NextProperty(JSContext *cx, JSObject *iterobj, jsid *idp)
{
    jsint i;
    JSObject *obj;
    JSScope *scope;
    JSScopeProperty *sprop;
    JSIdArray *ida;

    CHECK_REQUEST(cx);
    i = JSVAL_TO_INT(OBJ_GET_SLOT(cx, iterobj, JSSLOT_ITER_INDEX));
    if (i < 0) {
        /* Native case: private data is a property tree node pointer. */
        obj = OBJ_GET_PARENT(cx, iterobj);
        JS_ASSERT(OBJ_IS_NATIVE(obj));
        scope = OBJ_SCOPE(obj);
        JS_ASSERT(scope->object == obj);
        sprop = (JSScopeProperty *) JS_GetPrivate(cx, iterobj);

        /*
         * If the next property mapped by scope in the property tree ancestor
         * line is not enumerable, or it's an alias, or one or more properties
         * were deleted from the "middle" of the scope-mapped ancestor line
         * and the next property was among those deleted, skip it and keep on
         * trying to find an enumerable property that is still in scope.
         */
        while (sprop &&
               (!(sprop->attrs & JSPROP_ENUMERATE) ||
                (sprop->flags & SPROP_IS_ALIAS) ||
                (SCOPE_HAD_MIDDLE_DELETE(scope) &&
                 !SCOPE_HAS_PROPERTY(scope, sprop)))) {
            sprop = sprop->parent;
        }

        if (!sprop) {
            *idp = JSVAL_VOID;
        } else {
            if (!JS_SetPrivate(cx, iterobj, sprop->parent))
                return JS_FALSE;
            *idp = sprop->id;
        }
    } else {
        /* Non-native case: use the ida enumerated when iterobj was created. */
        ida = (JSIdArray *) JS_GetPrivate(cx, iterobj);
        JS_ASSERT(i <= ida->length);
        if (i == 0) {
            *idp = JSVAL_VOID;
        } else {
            *idp = ida->vector[--i];
            OBJ_SET_SLOT(cx, iterobj, JSSLOT_ITER_INDEX, INT_TO_JSVAL(i));
        }
    }
    return JS_TRUE;
}

JS_PUBLIC_API(JSBool)
JS_CheckAccess(JSContext *cx, JSObject *obj, jsid id, JSAccessMode mode,
               jsval *vp, uintN *attrsp)
{
    CHECK_REQUEST(cx);
    return OBJ_CHECK_ACCESS(cx, obj, id, mode, vp, attrsp);
}

JS_PUBLIC_API(JSCheckAccessOp)
JS_SetCheckObjectAccessCallback(JSRuntime *rt, JSCheckAccessOp acb)
{
    JSCheckAccessOp oldacb;

    oldacb = rt->checkObjectAccess;
    rt->checkObjectAccess = acb;
    return oldacb;
}

static JSBool
ReservedSlotIndexOK(JSContext *cx, JSObject *obj, JSClass *clasp,
                    uint32 index, uint32 limit)
{
    /* Check the computed, possibly per-instance, upper bound. */
    if (clasp->reserveSlots)
        JS_LOCK_OBJ_VOID(cx, obj, limit += clasp->reserveSlots(cx, obj));
    if (index >= limit) {
        JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL,
                             JSMSG_RESERVED_SLOT_RANGE);
        return JS_FALSE;
    }
    return JS_TRUE;
}

JS_PUBLIC_API(JSBool)
JS_GetReservedSlot(JSContext *cx, JSObject *obj, uint32 index, jsval *vp)
{
    JSClass *clasp;
    uint32 limit, slot;

    CHECK_REQUEST(cx);
    clasp = OBJ_GET_CLASS(cx, obj);
    limit = JSCLASS_RESERVED_SLOTS(clasp);
    if (index >= limit && !ReservedSlotIndexOK(cx, obj, clasp, index, limit))
        return JS_FALSE;
    slot = JSSLOT_START(clasp) + index;
    *vp = OBJ_GET_REQUIRED_SLOT(cx, obj, slot);
    return JS_TRUE;
}

JS_PUBLIC_API(JSBool)
JS_SetReservedSlot(JSContext *cx, JSObject *obj, uint32 index, jsval v)
{
    JSClass *clasp;
    uint32 limit, slot;

    CHECK_REQUEST(cx);
    clasp = OBJ_GET_CLASS(cx, obj);
    limit = JSCLASS_RESERVED_SLOTS(clasp);
    if (index >= limit && !ReservedSlotIndexOK(cx, obj, clasp, index, limit))
        return JS_FALSE;
    slot = JSSLOT_START(clasp) + index;
    return OBJ_SET_REQUIRED_SLOT(cx, obj, slot, v);
}

#ifdef JS_THREADSAFE
JS_PUBLIC_API(jsrefcount)
JS_HoldPrincipals(JSContext *cx, JSPrincipals *principals)
{
    return JS_ATOMIC_INCREMENT(&principals->refcount);
}

JS_PUBLIC_API(jsrefcount)
JS_DropPrincipals(JSContext *cx, JSPrincipals *principals)
{
    jsrefcount rc = JS_ATOMIC_DECREMENT(&principals->refcount);
    if (rc == 0)
        principals->destroy(cx, principals);
    return rc;
}
#endif

JS_PUBLIC_API(JSPrincipalsTranscoder)
JS_SetPrincipalsTranscoder(JSRuntime *rt, JSPrincipalsTranscoder px)
{
    JSPrincipalsTranscoder oldpx;

    oldpx = rt->principalsTranscoder;
    rt->principalsTranscoder = px;
    return oldpx;
}

JS_PUBLIC_API(JSObjectPrincipalsFinder)
JS_SetObjectPrincipalsFinder(JSRuntime *rt, JSObjectPrincipalsFinder fop)
{
    JSObjectPrincipalsFinder oldfop;

    oldfop = rt->findObjectPrincipals;
    rt->findObjectPrincipals = fop;
    return oldfop;
}

JS_PUBLIC_API(JSFunction *)
JS_NewFunction(JSContext *cx, JSNative native, uintN nargs, uintN flags,
               JSObject *parent, const char *name)
{
    JSAtom *atom;

    CHECK_REQUEST(cx);

    if (!name) {
        atom = NULL;
    } else {
        atom = js_Atomize(cx, name, strlen(name), 0);
        if (!atom)
            return NULL;
    }
    return js_NewFunction(cx, NULL, native, nargs, flags, parent, atom);
}

JS_PUBLIC_API(JSObject *)
JS_CloneFunctionObject(JSContext *cx, JSObject *funobj, JSObject *parent)
{
    CHECK_REQUEST(cx);
    if (OBJ_GET_CLASS(cx, funobj) != &js_FunctionClass) {
        /* Indicate we cannot clone this object. */
        return funobj;
    }
    return js_CloneFunctionObject(cx, funobj, parent);
}

JS_PUBLIC_API(JSObject *)
JS_GetFunctionObject(JSFunction *fun)
{
    return fun->object;
}

JS_PUBLIC_API(const char *)
JS_GetFunctionName(JSFunction *fun)
{
    return fun->atom
           ? JS_GetStringBytes(ATOM_TO_STRING(fun->atom))
           : js_anonymous_str;
}

JS_PUBLIC_API(JSString *)
JS_GetFunctionId(JSFunction *fun)
{
    return fun->atom ? ATOM_TO_STRING(fun->atom) : NULL;
}

JS_PUBLIC_API(uintN)
JS_GetFunctionFlags(JSFunction *fun)
{
#ifdef MOZILLA_1_8_BRANCH
    uintN flags = fun->flags;

    return JSFUN_DISJOINT_FLAGS(flags) |
           (JSFUN_GETTER_TEST(flags) ? JSFUN_GETTER : 0) |
           (JSFUN_SETTER_TEST(flags) ? JSFUN_SETTER : 0) |
           (JSFUN_BOUND_METHOD_TEST(flags) ? JSFUN_BOUND_METHOD : 0) |
           (JSFUN_HEAVYWEIGHT_TEST(flags) ? JSFUN_HEAVYWEIGHT : 0);
#else
    return fun->flags;
#endif
}

JS_PUBLIC_API(uint16)
JS_GetFunctionArity(JSFunction *fun)
{
    return fun->nargs;
}

JS_PUBLIC_API(JSBool)
JS_ObjectIsFunction(JSContext *cx, JSObject *obj)
{
    return OBJ_GET_CLASS(cx, obj) == &js_FunctionClass;
}

JS_STATIC_DLL_CALLBACK(JSBool)
js_generic_native_method_dispatcher(JSContext *cx, JSObject *obj,
                                    uintN argc, jsval *argv, jsval *rval)
{
    jsval fsv;
    JSFunctionSpec *fs;
    JSObject *tmp;

    if (!JS_GetReservedSlot(cx, JSVAL_TO_OBJECT(argv[-2]), 0, &fsv))
        return JS_FALSE;
    fs = (JSFunctionSpec *) JSVAL_TO_PRIVATE(fsv);

    /*
     * We know that argv[0] is valid because JS_DefineFunctions, which is our
     * only (indirect) referrer, defined us as requiring at least one argument
     * (notice how it passes fs->nargs + 1 as the next-to-last argument to
     * JS_DefineFunction).
     */
    if (JSVAL_IS_PRIMITIVE(argv[0])) {
        /*
         * Make sure that this is an object or null, as required by the generic
         * functions.
         */
        if (!js_ValueToObject(cx, argv[0], &tmp))
            return JS_FALSE;
        argv[0] = OBJECT_TO_JSVAL(tmp);
    }

    /*
     * Copy all actual (argc) and required but missing (fs->nargs + 1 - argc)
     * args down over our |this| parameter, argv[-1], which is almost always
     * the class constructor object, e.g. Array.  Then call the corresponding
     * prototype native method with our first argument passed as |this|.
     */
    memmove(argv - 1, argv, JS_MAX(fs->nargs + 1U, argc) * sizeof(jsval));

    /*
     * Follow Function.prototype.apply and .call by using the global object as
     * the 'this' param if no args.
     */
    JS_ASSERT(cx->fp->argv == argv);
    tmp = js_ComputeThis(cx, JSVAL_TO_OBJECT(argv[-1]), argv);
    if (!tmp)
        return JS_FALSE;
    cx->fp->thisp = tmp;

    /*
     * Protect against argc - 1 underflowing below. By calling js_ComputeThis,
     * we made it as if the static was called with one parameter.
     */
    if (argc == 0)
        argc = 1;

    return fs->call(cx, JSVAL_TO_OBJECT(argv[-1]), argc - 1, argv, rval);
}

JS_PUBLIC_API(JSBool)
JS_DefineFunctions(JSContext *cx, JSObject *obj, JSFunctionSpec *fs)
{
    uintN flags;
    JSObject *ctor;
    JSFunction *fun;

    CHECK_REQUEST(cx);
    ctor = NULL;
    for (; fs->name; fs++) {

        /* High bits of fs->extra are reserved. */
        JS_ASSERT((fs->extra & 0xFFFF0000) == 0);
        flags = fs->flags;

        /*
         * Define a generic arity N+1 static method for the arity N prototype
         * method if flags contains JSFUN_GENERIC_NATIVE.
         */
        if (flags & JSFUN_GENERIC_NATIVE) {
            if (!ctor) {
                ctor = JS_GetConstructor(cx, obj);
                if (!ctor)
                    return JS_FALSE;
            }

            flags &= ~JSFUN_GENERIC_NATIVE;
            fun = JS_DefineFunction(cx, ctor, fs->name,
                                    js_generic_native_method_dispatcher,
                                    fs->nargs + 1, flags);
            if (!fun)
                return JS_FALSE;
            fun->u.n.extra = (uint16)fs->extra;

            /*
             * As jsapi.h notes, fs must point to storage that lives as long
             * as fun->object lives.
             */
            if (!JS_SetReservedSlot(cx, fun->object, 0, PRIVATE_TO_JSVAL(fs)))
                return JS_FALSE;
        }

        fun = JS_DefineFunction(cx, obj, fs->name, fs->call, fs->nargs, flags);
        if (!fun)
            return JS_FALSE;
        fun->u.n.extra = (uint16)fs->extra;
    }
    return JS_TRUE;
}

JS_PUBLIC_API(JSFunction *)
JS_DefineFunction(JSContext *cx, JSObject *obj, const char *name, JSNative call,
                  uintN nargs, uintN attrs)
{
    JSAtom *atom;

    CHECK_REQUEST(cx);
    atom = js_Atomize(cx, name, strlen(name), 0);
    if (!atom)
        return NULL;
    return js_DefineFunction(cx, obj, atom, call, nargs, attrs);
}

JS_PUBLIC_API(JSFunction *)
JS_DefineUCFunction(JSContext *cx, JSObject *obj,
                    const jschar *name, size_t namelen, JSNative call,
                    uintN nargs, uintN attrs)
{
    JSAtom *atom;

    atom = js_AtomizeChars(cx, name, AUTO_NAMELEN(name, namelen), 0);
    if (!atom)
        return NULL;
    return js_DefineFunction(cx, obj, atom, call, nargs, attrs);
}

static JSScript *
CompileTokenStream(JSContext *cx, JSObject *obj, JSTokenStream *ts,
                   void *tempMark, JSBool *eofp)
{
    JSBool eof;
    JSArenaPool codePool, notePool;
    JSCodeGenerator cg;
    JSScript *script;

    CHECK_REQUEST(cx);
    eof = JS_FALSE;
    JS_InitArenaPool(&codePool, "code", 1024, sizeof(jsbytecode));
    JS_InitArenaPool(&notePool, "note", 1024, sizeof(jssrcnote));
    if (!js_InitCodeGenerator(cx, &cg, &codePool, &notePool,
                              ts->filename, ts->lineno,
                              ts->principals)) {
        script = NULL;
    } else if (!js_CompileTokenStream(cx, obj, ts, &cg)) {
        script = NULL;
        eof = (ts->flags & TSF_EOF) != 0;
    } else {
        script = js_NewScriptFromCG(cx, &cg, NULL);
    }
    if (eofp)
        *eofp = eof;
    if (!js_CloseTokenStream(cx, ts)) {
        if (script)
            js_DestroyScript(cx, script);
        script = NULL;
    }
    cg.tempMark = tempMark;
    js_FinishCodeGenerator(cx, &cg);
    JS_FinishArenaPool(&codePool);
    JS_FinishArenaPool(&notePool);
    return script;
}

JS_PUBLIC_API(JSScript *)
JS_CompileScript(JSContext *cx, JSObject *obj,
                 const char *bytes, size_t length,
                 const char *filename, uintN lineno)
{
    jschar *chars;
    JSScript *script;

    CHECK_REQUEST(cx);
    chars = js_InflateString(cx, bytes, &length);
    if (!chars)
        return NULL;
    script = JS_CompileUCScript(cx, obj, chars, length, filename, lineno);
    JS_free(cx, chars);
    return script;
}

JS_PUBLIC_API(JSScript *)
JS_CompileScriptForPrincipals(JSContext *cx, JSObject *obj,
                              JSPrincipals *principals,
                              const char *bytes, size_t length,
                              const char *filename, uintN lineno)
{
    jschar *chars;
    JSScript *script;

    CHECK_REQUEST(cx);
    chars = js_InflateString(cx, bytes, &length);
    if (!chars)
        return NULL;
    script = JS_CompileUCScriptForPrincipals(cx, obj, principals,
                                             chars, length, filename, lineno);
    JS_free(cx, chars);
    return script;
}

JS_PUBLIC_API(JSScript *)
JS_CompileUCScript(JSContext *cx, JSObject *obj,
                   const jschar *chars, size_t length,
                   const char *filename, uintN lineno)
{
    CHECK_REQUEST(cx);
    return JS_CompileUCScriptForPrincipals(cx, obj, NULL, chars, length,
                                           filename, lineno);
}

#define LAST_FRAME_EXCEPTION_CHECK(cx,result)                                 \
    JS_BEGIN_MACRO                                                            \
        if (!(result) && !((cx)->options & JSOPTION_DONT_REPORT_UNCAUGHT))    \
            js_ReportUncaughtException(cx);                                   \
    JS_END_MACRO

#define LAST_FRAME_CHECKS(cx,result)                                          \
    JS_BEGIN_MACRO                                                            \
        if (!(cx)->fp) {                                                      \
            (cx)->weakRoots.lastInternalResult = JSVAL_NULL;                  \
            LAST_FRAME_EXCEPTION_CHECK(cx, result);                           \
        }                                                                     \
    JS_END_MACRO

JS_PUBLIC_API(JSScript *)
JS_CompileUCScriptForPrincipals(JSContext *cx, JSObject *obj,
                                JSPrincipals *principals,
                                const jschar *chars, size_t length,
                                const char *filename, uintN lineno)
{
    void *mark;
    JSTokenStream *ts;
    JSScript *script;

    CHECK_REQUEST(cx);
    mark = JS_ARENA_MARK(&cx->tempPool);
    ts = js_NewTokenStream(cx, chars, length, filename, lineno, principals);
    if (!ts)
        return NULL;
    script = CompileTokenStream(cx, obj, ts, mark, NULL);
    LAST_FRAME_CHECKS(cx, script);
    return script;
}

JS_PUBLIC_API(JSBool)
JS_BufferIsCompilableUnit(JSContext *cx, JSObject *obj,
                          const char *bytes, size_t length)
{
    jschar *chars;
    JSBool result;
    JSExceptionState *exnState;
    void *tempMark;
    JSTokenStream *ts;
    JSErrorReporter older;

    CHECK_REQUEST(cx);
    chars = js_InflateString(cx, bytes, &length);
    if (!chars)
        return JS_TRUE;

    /*
     * Return true on any out-of-memory error, so our caller doesn't try to
     * collect more buffered source.
     */
    result = JS_TRUE;
    exnState = JS_SaveExceptionState(cx);
    tempMark = JS_ARENA_MARK(&cx->tempPool);
    ts = js_NewTokenStream(cx, chars, length, NULL, 0, NULL);
    if (ts) {
        older = JS_SetErrorReporter(cx, NULL);
        if (!js_ParseTokenStream(cx, obj, ts) &&
            (ts->flags & TSF_UNEXPECTED_EOF)) {
            /*
             * We ran into an error.  If it was because we ran out of source,
             * we return false, so our caller will know to try to collect more
             * buffered source.
             */
            result = JS_FALSE;
        }

        JS_SetErrorReporter(cx, older);
        js_CloseTokenStream(cx, ts);
        JS_ARENA_RELEASE(&cx->tempPool, tempMark);
    }

    JS_free(cx, chars);
    JS_RestoreExceptionState(cx, exnState);
    return result;
}

JS_PUBLIC_API(JSScript *)
JS_CompileFile(JSContext *cx, JSObject *obj, const char *filename)
{
    void *mark;
    JSTokenStream *ts;
    JSScript *script;

    CHECK_REQUEST(cx);
    mark = JS_ARENA_MARK(&cx->tempPool);
    ts = js_NewFileTokenStream(cx, filename, stdin);
    if (!ts)
        return NULL;
    script = CompileTokenStream(cx, obj, ts, mark, NULL);
    LAST_FRAME_CHECKS(cx, script);
    return script;
}

JS_PUBLIC_API(JSScript *)
JS_CompileFileHandle(JSContext *cx, JSObject *obj, const char *filename,
                     FILE *file)
{
    return JS_CompileFileHandleForPrincipals(cx, obj, filename, file, NULL);
}

JS_PUBLIC_API(JSScript *)
JS_CompileFileHandleForPrincipals(JSContext *cx, JSObject *obj,
                                  const char *filename, FILE *file,
                                  JSPrincipals *principals)
{
    void *mark;
    JSTokenStream *ts;
    JSScript *script;

    CHECK_REQUEST(cx);
    mark = JS_ARENA_MARK(&cx->tempPool);
    ts = js_NewFileTokenStream(cx, NULL, file);
    if (!ts)
        return NULL;
    ts->filename = filename;
    /* XXXshaver js_NewFileTokenStream should do this, because it drops */
    if (principals) {
        ts->principals = principals;
        JSPRINCIPALS_HOLD(cx, ts->principals);
    }
    script = CompileTokenStream(cx, obj, ts, mark, NULL);
    LAST_FRAME_CHECKS(cx, script);
    return script;
}

JS_PUBLIC_API(JSObject *)
JS_NewScriptObject(JSContext *cx, JSScript *script)
{
    JSObject *obj;

    obj = js_NewObject(cx, &js_ScriptClass, NULL, NULL);
    if (!obj)
        return NULL;

    if (script) {
        if (!JS_SetPrivate(cx, obj, script))
            return NULL;
        script->object = obj;
    }
    return obj;
}

JS_PUBLIC_API(JSObject *)
JS_GetScriptObject(JSScript *script)
{
    return script->object;
}

JS_PUBLIC_API(void)
JS_DestroyScript(JSContext *cx, JSScript *script)
{
    CHECK_REQUEST(cx);
    js_DestroyScript(cx, script);
}

JS_PUBLIC_API(JSFunction *)
JS_CompileFunction(JSContext *cx, JSObject *obj, const char *name,
                   uintN nargs, const char **argnames,
                   const char *bytes, size_t length,
                   const char *filename, uintN lineno)
{
    jschar *chars;
    JSFunction *fun;

    CHECK_REQUEST(cx);
    chars = js_InflateString(cx, bytes, &length);
    if (!chars)
        return NULL;
    fun = JS_CompileUCFunction(cx, obj, name, nargs, argnames, chars, length,
                               filename, lineno);
    JS_free(cx, chars);
    return fun;
}

JS_PUBLIC_API(JSFunction *)
JS_CompileFunctionForPrincipals(JSContext *cx, JSObject *obj,
                                JSPrincipals *principals, const char *name,
                                uintN nargs, const char **argnames,
                                const char *bytes, size_t length,
                                const char *filename, uintN lineno)
{
    jschar *chars;
    JSFunction *fun;

    CHECK_REQUEST(cx);
    chars = js_InflateString(cx, bytes, &length);
    if (!chars)
        return NULL;
    fun = JS_CompileUCFunctionForPrincipals(cx, obj, principals, name,
                                            nargs, argnames, chars, length,
                                            filename, lineno);
    JS_free(cx, chars);
    return fun;
}

JS_PUBLIC_API(JSFunction *)
JS_CompileUCFunction(JSContext *cx, JSObject *obj, const char *name,
                     uintN nargs, const char **argnames,
                     const jschar *chars, size_t length,
                     const char *filename, uintN lineno)
{
    CHECK_REQUEST(cx);
    return JS_CompileUCFunctionForPrincipals(cx, obj, NULL, name,
                                             nargs, argnames,
                                             chars, length,
                                             filename, lineno);
}

JS_PUBLIC_API(JSFunction *)
JS_CompileUCFunctionForPrincipals(JSContext *cx, JSObject *obj,
                                  JSPrincipals *principals, const char *name,
                                  uintN nargs, const char **argnames,
                                  const jschar *chars, size_t length,
                                  const char *filename, uintN lineno)
{
    void *mark;
    JSTokenStream *ts;
    JSFunction *fun;
    JSAtom *funAtom, *argAtom;
    uintN i;

    CHECK_REQUEST(cx);
    mark = JS_ARENA_MARK(&cx->tempPool);
    ts = js_NewTokenStream(cx, chars, length, filename, lineno, principals);
    if (!ts) {
        fun = NULL;
        goto out;
    }
    if (!name) {
        funAtom = NULL;
    } else {
        funAtom = js_Atomize(cx, name, strlen(name), 0);
        if (!funAtom) {
            fun = NULL;
            goto out;
        }
    }
    fun = js_NewFunction(cx, NULL, NULL, nargs, 0, obj, funAtom);
    if (!fun)
        goto out;
    if (nargs) {
        for (i = 0; i < nargs; i++) {
            argAtom = js_Atomize(cx, argnames[i], strlen(argnames[i]), 0);
            if (!argAtom)
                break;
            if (!js_AddHiddenProperty(cx, fun->object, ATOM_TO_JSID(argAtom),
                                      js_GetArgument, js_SetArgument,
                                      SPROP_INVALID_SLOT,
                                      JSPROP_PERMANENT | JSPROP_SHARED,
                                      SPROP_HAS_SHORTID, i)) {
                break;
            }
        }
        if (i < nargs) {
            fun = NULL;
            goto out;
        }
    }
    if (!js_CompileFunctionBody(cx, ts, fun)) {
        fun = NULL;
        goto out;
    }
    if (obj && funAtom) {
        if (!OBJ_DEFINE_PROPERTY(cx, obj, ATOM_TO_JSID(funAtom),
                                 OBJECT_TO_JSVAL(fun->object),
                                 NULL, NULL, JSPROP_ENUMERATE, NULL)) {
            return NULL;
        }
    }
out:
    if (ts)
        js_CloseTokenStream(cx, ts);
    JS_ARENA_RELEASE(&cx->tempPool, mark);
    LAST_FRAME_CHECKS(cx, fun);
    return fun;
}

JS_PUBLIC_API(JSString *)
JS_DecompileScript(JSContext *cx, JSScript *script, const char *name,
                   uintN indent)
{
    JSPrinter *jp;
    JSString *str;

    CHECK_REQUEST(cx);
    jp = js_NewPrinter(cx, name,
                       indent & ~JS_DONT_PRETTY_PRINT,
                       !(indent & JS_DONT_PRETTY_PRINT));
    if (!jp)
        return NULL;
    if (js_DecompileScript(jp, script))
        str = js_GetPrinterOutput(jp);
    else
        str = NULL;
    js_DestroyPrinter(jp);
    return str;
}

JS_PUBLIC_API(JSString *)
JS_DecompileFunction(JSContext *cx, JSFunction *fun, uintN indent)
{
    JSPrinter *jp;
    JSString *str;

    CHECK_REQUEST(cx);
    jp = js_NewPrinter(cx, JS_GetFunctionName(fun),
                       indent & ~JS_DONT_PRETTY_PRINT,
                       !(indent & JS_DONT_PRETTY_PRINT));
    if (!jp)
        return NULL;
    if (js_DecompileFunction(jp, fun))
        str = js_GetPrinterOutput(jp);
    else
        str = NULL;
    js_DestroyPrinter(jp);
    return str;
}

JS_PUBLIC_API(JSString *)
JS_DecompileFunctionBody(JSContext *cx, JSFunction *fun, uintN indent)
{
    JSPrinter *jp;
    JSString *str;

    CHECK_REQUEST(cx);
    jp = js_NewPrinter(cx, JS_GetFunctionName(fun),
                       indent & ~JS_DONT_PRETTY_PRINT,
                       !(indent & JS_DONT_PRETTY_PRINT));
    if (!jp)
        return NULL;
    if (js_DecompileFunctionBody(jp, fun))
        str = js_GetPrinterOutput(jp);
    else
        str = NULL;
    js_DestroyPrinter(jp);
    return str;
}

JS_PUBLIC_API(JSBool)
JS_ExecuteScript(JSContext *cx, JSObject *obj, JSScript *script, jsval *rval)
{
    JSBool ok;

    CHECK_REQUEST(cx);
    ok = js_Execute(cx, obj, script, NULL, 0, rval);
    LAST_FRAME_CHECKS(cx, ok);
    return ok;
}

JS_PUBLIC_API(JSBool)
JS_ExecuteScriptPart(JSContext *cx, JSObject *obj, JSScript *script,
                     JSExecPart part, jsval *rval)
{
    JSScript tmp;
    JSRuntime *rt;
    JSBool ok;

    /* Make a temporary copy of the JSScript structure and farble it a bit. */
    tmp = *script;
    if (part == JSEXEC_PROLOG) {
        tmp.length = PTRDIFF(tmp.main, tmp.code, jsbytecode);
    } else {
        tmp.length -= PTRDIFF(tmp.main, tmp.code, jsbytecode);
        tmp.code = tmp.main;
    }

    /* Tell the debugger about our temporary copy of the script structure. */
    rt = cx->runtime;
    if (rt->newScriptHook) {
        rt->newScriptHook(cx, tmp.filename, tmp.lineno, &tmp, NULL,
                          rt->newScriptHookData);
    }

    /* Execute the farbled struct and tell the debugger to forget about it. */
    ok = JS_ExecuteScript(cx, obj, &tmp, rval);
    if (rt->destroyScriptHook)
        rt->destroyScriptHook(cx, &tmp, rt->destroyScriptHookData);
    return ok;
}

/* Ancient uintN nbytes is part of API/ABI, so use size_t length local. */
JS_PUBLIC_API(JSBool)
JS_EvaluateScript(JSContext *cx, JSObject *obj,
                  const char *bytes, uintN nbytes,
                  const char *filename, uintN lineno,
                  jsval *rval)
{
    size_t length = nbytes;
    jschar *chars;
    JSBool ok;

    CHECK_REQUEST(cx);
    chars = js_InflateString(cx, bytes, &length);
    if (!chars)
        return JS_FALSE;
    ok = JS_EvaluateUCScript(cx, obj, chars, length, filename, lineno, rval);
    JS_free(cx, chars);
    return ok;
}

/* Ancient uintN nbytes is part of API/ABI, so use size_t length local. */
JS_PUBLIC_API(JSBool)
JS_EvaluateScriptForPrincipals(JSContext *cx, JSObject *obj,
                               JSPrincipals *principals,
                               const char *bytes, uintN nbytes,
                               const char *filename, uintN lineno,
                               jsval *rval)
{
    size_t length = nbytes;
    jschar *chars;
    JSBool ok;

    CHECK_REQUEST(cx);
    chars = js_InflateString(cx, bytes, &length);
    if (!chars)
        return JS_FALSE;
    ok = JS_EvaluateUCScriptForPrincipals(cx, obj, principals, chars, length,
                                          filename, lineno, rval);
    JS_free(cx, chars);
    return ok;
}

JS_PUBLIC_API(JSBool)
JS_EvaluateUCScript(JSContext *cx, JSObject *obj,
                    const jschar *chars, uintN length,
                    const char *filename, uintN lineno,
                    jsval *rval)
{
    CHECK_REQUEST(cx);
    return JS_EvaluateUCScriptForPrincipals(cx, obj, NULL, chars, length,
                                            filename, lineno, rval);
}

JS_PUBLIC_API(JSBool)
JS_EvaluateUCScriptForPrincipals(JSContext *cx, JSObject *obj,
                                 JSPrincipals *principals,
                                 const jschar *chars, uintN length,
                                 const char *filename, uintN lineno,
                                 jsval *rval)
{
    uint32 options;
    JSScript *script;
    JSBool ok;

    CHECK_REQUEST(cx);
    options = cx->options;
    cx->options = options | JSOPTION_COMPILE_N_GO;
    script = JS_CompileUCScriptForPrincipals(cx, obj, principals, chars, length,
                                             filename, lineno);
    cx->options = options;
    if (!script)
        return JS_FALSE;
    ok = js_Execute(cx, obj, script, NULL, 0, rval);
    LAST_FRAME_CHECKS(cx, ok);
    JS_DestroyScript(cx, script);
    return ok;
}

JS_PUBLIC_API(JSBool)
JS_CallFunction(JSContext *cx, JSObject *obj, JSFunction *fun, uintN argc,
                jsval *argv, jsval *rval)
{
    JSBool ok;

    CHECK_REQUEST(cx);
    ok = js_InternalCall(cx, obj, OBJECT_TO_JSVAL(fun->object), argc, argv,
                         rval);
    LAST_FRAME_CHECKS(cx, ok);
    return ok;
}

JS_PUBLIC_API(JSBool)
JS_CallFunctionName(JSContext *cx, JSObject *obj, const char *name, uintN argc,
                    jsval *argv, jsval *rval)
{
    JSBool ok;
    jsval fval;

    CHECK_REQUEST(cx);
#if JS_HAS_XML_SUPPORT
    if (OBJECT_IS_XML(cx, obj)) {
        JSXMLObjectOps *ops;
        JSAtom *atom;

        ops = (JSXMLObjectOps *) obj->map->ops;
        atom = js_Atomize(cx, name, strlen(name), 0);
        if (!atom)
            return JS_FALSE;
        obj = ops->getMethod(cx, obj, ATOM_TO_JSID(atom), &fval);
        if (!obj)
            return JS_FALSE;
    } else
#endif
    if (!JS_GetProperty(cx, obj, name, &fval))
        return JS_FALSE;
    ok = js_InternalCall(cx, obj, fval, argc, argv, rval);
    LAST_FRAME_CHECKS(cx, ok);
    return ok;
}

JS_PUBLIC_API(JSBool)
JS_CallFunctionValue(JSContext *cx, JSObject *obj, jsval fval, uintN argc,
                     jsval *argv, jsval *rval)
{
    JSBool ok;

    CHECK_REQUEST(cx);
    ok = js_InternalCall(cx, obj, fval, argc, argv, rval);
    LAST_FRAME_CHECKS(cx, ok);
    return ok;
}

JS_PUBLIC_API(JSBranchCallback)
JS_SetBranchCallback(JSContext *cx, JSBranchCallback cb)
{
    JSBranchCallback oldcb;

    oldcb = cx->branchCallback;
    cx->branchCallback = cb;
    return oldcb;
}

JS_PUBLIC_API(JSBool)
JS_IsRunning(JSContext *cx)
{
    return cx->fp != NULL;
}

JS_PUBLIC_API(JSBool)
JS_IsConstructing(JSContext *cx)
{
    return cx->fp && (cx->fp->flags & JSFRAME_CONSTRUCTING);
}

JS_FRIEND_API(JSBool)
JS_IsAssigning(JSContext *cx)
{
    JSStackFrame *fp;
    jsbytecode *pc;

    for (fp = cx->fp; fp && !fp->script; fp = fp->down)
        continue;
    if (!fp || !(pc = fp->pc))
        return JS_FALSE;
    return (js_CodeSpec[*pc].format & JOF_ASSIGNING) != 0;
}

JS_PUBLIC_API(void)
JS_SetCallReturnValue2(JSContext *cx, jsval v)
{
#if JS_HAS_LVALUE_RETURN
    cx->rval2 = v;
    cx->rval2set = JS_TRUE;
#endif
}

JS_PUBLIC_API(JSStackFrame *)
JS_SaveFrameChain(JSContext *cx)
{
    JSStackFrame *fp;

    fp = cx->fp;
    if (!fp)
        return fp;

    JS_ASSERT(!fp->dormantNext);
    fp->dormantNext = cx->dormantFrameChain;
    cx->dormantFrameChain = fp;
    cx->fp = NULL;
    return fp;
}

JS_PUBLIC_API(void)
JS_RestoreFrameChain(JSContext *cx, JSStackFrame *fp)
{
    JS_ASSERT(!cx->fp);
    if (!fp)
        return;

    JS_ASSERT(cx->dormantFrameChain == fp);
    cx->fp = fp;
    cx->dormantFrameChain = fp->dormantNext;
    fp->dormantNext = NULL;
}

/************************************************************************/

JS_PUBLIC_API(JSString *)
JS_NewString(JSContext *cx, char *bytes, size_t nbytes)
{
    size_t length = nbytes;
    jschar *chars;
    JSString *str;

    CHECK_REQUEST(cx);

    /* Make a UTF-16 vector from the 8-bit char codes in bytes. */
    chars = js_InflateString(cx, bytes, &length);
    if (!chars)
        return NULL;

    /* Free chars (but not bytes, which caller frees on error) if we fail. */
    str = js_NewString(cx, chars, length, 0);
    if (!str) {
        JS_free(cx, chars);
        return NULL;
    }

    /* Hand off bytes to the deflated string cache, if possible. */
    if (!js_SetStringBytes(cx->runtime, str, bytes, nbytes))
        JS_free(cx, bytes);
    return str;
}

JS_PUBLIC_API(JSString *)
JS_NewStringCopyN(JSContext *cx, const char *s, size_t n)
{
    jschar *js;
    JSString *str;

    CHECK_REQUEST(cx);
    js = js_InflateString(cx, s, &n);
    if (!js)
        return NULL;
    str = js_NewString(cx, js, n, 0);
    if (!str)
        JS_free(cx, js);
    return str;
}

JS_PUBLIC_API(JSString *)
JS_NewStringCopyZ(JSContext *cx, const char *s)
{
    size_t n;
    jschar *js;
    JSString *str;

    CHECK_REQUEST(cx);
    if (!s)
        return cx->runtime->emptyString;
    n = strlen(s);
    js = js_InflateString(cx, s, &n);
    if (!js)
        return NULL;
    str = js_NewString(cx, js, n, 0);
    if (!str)
        JS_free(cx, js);
    return str;
}

JS_PUBLIC_API(JSString *)
JS_InternString(JSContext *cx, const char *s)
{
    JSAtom *atom;

    CHECK_REQUEST(cx);
    atom = js_Atomize(cx, s, strlen(s), ATOM_INTERNED);
    if (!atom)
        return NULL;
    return ATOM_TO_STRING(atom);
}

JS_PUBLIC_API(JSString *)
JS_NewUCString(JSContext *cx, jschar *chars, size_t length)
{
    CHECK_REQUEST(cx);
    return js_NewString(cx, chars, length, 0);
}

JS_PUBLIC_API(JSString *)
JS_NewUCStringCopyN(JSContext *cx, const jschar *s, size_t n)
{
    CHECK_REQUEST(cx);
    return js_NewStringCopyN(cx, s, n, 0);
}

JS_PUBLIC_API(JSString *)
JS_NewUCStringCopyZ(JSContext *cx, const jschar *s)
{
    CHECK_REQUEST(cx);
    if (!s)
        return cx->runtime->emptyString;
    return js_NewStringCopyZ(cx, s, 0);
}

JS_PUBLIC_API(JSString *)
JS_InternUCStringN(JSContext *cx, const jschar *s, size_t length)
{
    JSAtom *atom;

    CHECK_REQUEST(cx);
    atom = js_AtomizeChars(cx, s, length, ATOM_INTERNED);
    if (!atom)
        return NULL;
    return ATOM_TO_STRING(atom);
}

JS_PUBLIC_API(JSString *)
JS_InternUCString(JSContext *cx, const jschar *s)
{
    return JS_InternUCStringN(cx, s, js_strlen(s));
}

JS_PUBLIC_API(char *)
JS_GetStringBytes(JSString *str)
{
    JSRuntime *rt;
    char *bytes;

    rt = js_GetGCStringRuntime(str);
    bytes = js_GetStringBytes(rt, str);
    return bytes ? bytes : "";
}

JS_PUBLIC_API(jschar *)
JS_GetStringChars(JSString *str)
{
    /*
     * API botch (again, shades of JS_GetStringBytes): we have no cx to pass
     * to js_UndependString (called by js_GetStringChars) for out-of-memory
     * error reports, so js_UndependString passes NULL and suppresses errors.
     * If it fails to convert a dependent string into an independent one, our
     * caller will not be guaranteed a \u0000 terminator as a backstop.  This
     * may break some clients who already misbehave on embedded NULs.
     *
     * The gain of dependent strings, which cure quadratic and cubic growth
     * rate bugs in string concatenation, is worth this slight loss in API
     * compatibility.
     */
    jschar *chars;

    chars = js_GetStringChars(str);
    return chars ? chars : JSSTRING_CHARS(str);
}

JS_PUBLIC_API(size_t)
JS_GetStringLength(JSString *str)
{
    return JSSTRING_LENGTH(str);
}

JS_PUBLIC_API(intN)
JS_CompareStrings(JSString *str1, JSString *str2)
{
    return js_CompareStrings(str1, str2);
}

JS_PUBLIC_API(JSString *)
JS_NewGrowableString(JSContext *cx, jschar *chars, size_t length)
{
    CHECK_REQUEST(cx);
    return js_NewString(cx, chars, length, GCF_MUTABLE);
}

JS_PUBLIC_API(JSString *)
JS_NewDependentString(JSContext *cx, JSString *str, size_t start,
                      size_t length)
{
    CHECK_REQUEST(cx);
    return js_NewDependentString(cx, str, start, length, 0);
}

JS_PUBLIC_API(JSString *)
JS_ConcatStrings(JSContext *cx, JSString *left, JSString *right)
{
    CHECK_REQUEST(cx);
    return js_ConcatStrings(cx, left, right);
}

JS_PUBLIC_API(const jschar *)
JS_UndependString(JSContext *cx, JSString *str)
{
    CHECK_REQUEST(cx);
    return js_UndependString(cx, str);
}

JS_PUBLIC_API(JSBool)
JS_MakeStringImmutable(JSContext *cx, JSString *str)
{
    CHECK_REQUEST(cx);
    if (!js_UndependString(cx, str))
        return JS_FALSE;

    *js_GetGCThingFlags(str) &= ~GCF_MUTABLE;
    return JS_TRUE;
}

JS_PUBLIC_API(JSBool)
JS_EncodeCharacters(JSContext *cx, const jschar *src, size_t srclen, char *dst,
                    size_t *dstlenp)
{
    return js_DeflateStringToBuffer(cx, src, srclen, dst, dstlenp);
}

JS_PUBLIC_API(JSBool)
JS_DecodeBytes(JSContext *cx, const char *src, size_t srclen, jschar *dst,
               size_t *dstlenp)
{
    return js_InflateStringToBuffer(cx, src, srclen, dst, dstlenp);
}

JS_PUBLIC_API(JSBool)
JS_CStringsAreUTF8()
{
#ifdef JS_C_STRINGS_ARE_UTF8
    return JS_TRUE;
#else
    return JS_FALSE;
#endif
}

/************************************************************************/

JS_PUBLIC_API(void)
JS_ReportError(JSContext *cx, const char *format, ...)
{
    va_list ap;

    va_start(ap, format);
    js_ReportErrorVA(cx, JSREPORT_ERROR, format, ap);
    va_end(ap);
}

JS_PUBLIC_API(void)
JS_ReportErrorNumber(JSContext *cx, JSErrorCallback errorCallback,
                     void *userRef, const uintN errorNumber, ...)
{
    va_list ap;

    va_start(ap, errorNumber);
    js_ReportErrorNumberVA(cx, JSREPORT_ERROR, errorCallback, userRef,
                           errorNumber, JS_TRUE, ap);
    va_end(ap);
}

JS_PUBLIC_API(void)
JS_ReportErrorNumberUC(JSContext *cx, JSErrorCallback errorCallback,
                     void *userRef, const uintN errorNumber, ...)
{
    va_list ap;

    va_start(ap, errorNumber);
    js_ReportErrorNumberVA(cx, JSREPORT_ERROR, errorCallback, userRef,
                           errorNumber, JS_FALSE, ap);
    va_end(ap);
}

JS_PUBLIC_API(JSBool)
JS_ReportWarning(JSContext *cx, const char *format, ...)
{
    va_list ap;
    JSBool ok;

    va_start(ap, format);
    ok = js_ReportErrorVA(cx, JSREPORT_WARNING, format, ap);
    va_end(ap);
    return ok;
}

JS_PUBLIC_API(JSBool)
JS_ReportErrorFlagsAndNumber(JSContext *cx, uintN flags,
                             JSErrorCallback errorCallback, void *userRef,
                             const uintN errorNumber, ...)
{
    va_list ap;
    JSBool ok;

    va_start(ap, errorNumber);
    ok = js_ReportErrorNumberVA(cx, flags, errorCallback, userRef,
                                errorNumber, JS_TRUE, ap);
    va_end(ap);
    return ok;
}

JS_PUBLIC_API(JSBool)
JS_ReportErrorFlagsAndNumberUC(JSContext *cx, uintN flags,
                               JSErrorCallback errorCallback, void *userRef,
                               const uintN errorNumber, ...)
{
    va_list ap;
    JSBool ok;

    va_start(ap, errorNumber);
    ok = js_ReportErrorNumberVA(cx, flags, errorCallback, userRef,
                                errorNumber, JS_FALSE, ap);
    va_end(ap);
    return ok;
}

JS_PUBLIC_API(void)
JS_ReportOutOfMemory(JSContext *cx)
{
    js_ReportOutOfMemory(cx);
}

JS_PUBLIC_API(JSErrorReporter)
JS_SetErrorReporter(JSContext *cx, JSErrorReporter er)
{
    JSErrorReporter older;

    older = cx->errorReporter;
    cx->errorReporter = er;
    return older;
}

/************************************************************************/

/*
 * Regular Expressions.
 */
JS_PUBLIC_API(JSObject *)
JS_NewRegExpObject(JSContext *cx, char *bytes, size_t length, uintN flags)
{
    jschar *chars;
    JSObject *obj;

    CHECK_REQUEST(cx);
    chars = js_InflateString(cx, bytes, &length);
    if (!chars)
        return NULL;
    obj = js_NewRegExpObject(cx, NULL, chars, length, flags);
    JS_free(cx, chars);
    return obj;
}

JS_PUBLIC_API(JSObject *)
JS_NewUCRegExpObject(JSContext *cx, jschar *chars, size_t length, uintN flags)
{
    CHECK_REQUEST(cx);
    return js_NewRegExpObject(cx, NULL, chars, length, flags);
}

JS_PUBLIC_API(void)
JS_SetRegExpInput(JSContext *cx, JSString *input, JSBool multiline)
{
    JSRegExpStatics *res;

    CHECK_REQUEST(cx);
    /* No locking required, cx is thread-private and input must be live. */
    res = &cx->regExpStatics;
    res->input = input;
    res->multiline = multiline;
    cx->runtime->gcPoke = JS_TRUE;
}

JS_PUBLIC_API(void)
JS_ClearRegExpStatics(JSContext *cx)
{
    JSRegExpStatics *res;

    /* No locking required, cx is thread-private and input must be live. */
    res = &cx->regExpStatics;
    res->input = NULL;
    res->multiline = JS_FALSE;
    res->parenCount = 0;
    res->lastMatch = res->lastParen = js_EmptySubString;
    res->leftContext = res->rightContext = js_EmptySubString;
    cx->runtime->gcPoke = JS_TRUE;
}

JS_PUBLIC_API(void)
JS_ClearRegExpRoots(JSContext *cx)
{
    JSRegExpStatics *res;

    /* No locking required, cx is thread-private and input must be live. */
    res = &cx->regExpStatics;
    res->input = NULL;
    cx->runtime->gcPoke = JS_TRUE;
}

/* TODO: compile, execute, get/set other statics... */

/************************************************************************/

JS_PUBLIC_API(void)
JS_SetLocaleCallbacks(JSContext *cx, JSLocaleCallbacks *callbacks)
{
    cx->localeCallbacks = callbacks;
}

JS_PUBLIC_API(JSLocaleCallbacks *)
JS_GetLocaleCallbacks(JSContext *cx)
{
    return cx->localeCallbacks;
}

/************************************************************************/

JS_PUBLIC_API(JSBool)
JS_IsExceptionPending(JSContext *cx)
{
    return (JSBool) cx->throwing;
}

JS_PUBLIC_API(JSBool)
JS_GetPendingException(JSContext *cx, jsval *vp)
{
    CHECK_REQUEST(cx);
    if (!cx->throwing)
        return JS_FALSE;
    *vp = cx->exception;
    return JS_TRUE;
}

JS_PUBLIC_API(void)
JS_SetPendingException(JSContext *cx, jsval v)
{
    CHECK_REQUEST(cx);
    cx->throwing = JS_TRUE;
    cx->exception = v;
}

JS_PUBLIC_API(void)
JS_ClearPendingException(JSContext *cx)
{
    cx->throwing = JS_FALSE;
    cx->exception = JSVAL_VOID;
}

JS_PUBLIC_API(JSBool)
JS_ReportPendingException(JSContext *cx)
{
    JSBool save, ok;

    CHECK_REQUEST(cx);

    /*
     * Set cx->creatingException to suppress the standard error-to-exception
     * conversion done by all {js,JS}_Report* functions except for OOM.  The
     * cx->creatingException flag was added to suppress recursive divergence
     * under js_ErrorToException, but it serves for our purposes here too.
     */
    save = cx->creatingException;
    cx->creatingException = JS_TRUE;
    ok = js_ReportUncaughtException(cx);
    cx->creatingException = save;
    return ok;
}

struct JSExceptionState {
    JSBool throwing;
    jsval  exception;
};

JS_PUBLIC_API(JSExceptionState *)
JS_SaveExceptionState(JSContext *cx)
{
    JSExceptionState *state;

    CHECK_REQUEST(cx);
    state = (JSExceptionState *) JS_malloc(cx, sizeof(JSExceptionState));
    if (state) {
        state->throwing = JS_GetPendingException(cx, &state->exception);
        if (state->throwing && JSVAL_IS_GCTHING(state->exception))
            js_AddRoot(cx, &state->exception, "JSExceptionState.exception");
    }
    return state;
}

JS_PUBLIC_API(void)
JS_RestoreExceptionState(JSContext *cx, JSExceptionState *state)
{
    CHECK_REQUEST(cx);
    if (state) {
        if (state->throwing)
            JS_SetPendingException(cx, state->exception);
        else
            JS_ClearPendingException(cx);
        JS_DropExceptionState(cx, state);
    }
}

JS_PUBLIC_API(void)
JS_DropExceptionState(JSContext *cx, JSExceptionState *state)
{
    CHECK_REQUEST(cx);
    if (state) {
        if (state->throwing && JSVAL_IS_GCTHING(state->exception))
            JS_RemoveRoot(cx, &state->exception);
        JS_free(cx, state);
    }
}

JS_PUBLIC_API(JSErrorReport *)
JS_ErrorFromException(JSContext *cx, jsval v)
{
    CHECK_REQUEST(cx);
    return js_ErrorFromException(cx, v);
}

JS_PUBLIC_API(JSBool)
JS_ThrowReportedError(JSContext *cx, const char *message,
                      JSErrorReport *reportp)
{
    return js_ErrorToException(cx, message, reportp);
}

#ifdef JS_THREADSAFE
/*
 * Get the owning thread id of a context. Returns 0 if the context is not
 * owned by any thread.
 */
JS_PUBLIC_API(jsword)
JS_GetContextThread(JSContext *cx)
{
    return JS_THREAD_ID(cx);
}

/*
 * Set the current thread as the owning thread of a context. Returns the
 * old owning thread id, or -1 if the operation failed.
 */
JS_PUBLIC_API(jsword)
JS_SetContextThread(JSContext *cx)
{
    jsword old = JS_THREAD_ID(cx);
    if (!js_SetContextThread(cx))
        return -1;
    return old;
}

JS_PUBLIC_API(jsword)
JS_ClearContextThread(JSContext *cx)
{
    jsword old = JS_THREAD_ID(cx);
    js_ClearContextThread(cx);
    return old;
}
#endif

/************************************************************************/

#if defined(XP_WIN)
#include <windows.h>
/*
 * Initialization routine for the JS DLL...
 */

/*
 * Global Instance handle...
 * In Win32 this is the module handle of the DLL.
 *
 * In Win16 this is the instance handle of the application
 * which loaded the DLL.
 */

#ifdef _WIN32
BOOL WINAPI DllMain (HINSTANCE hDLL, DWORD dwReason, LPVOID lpReserved)
{
    return TRUE;
}

#else  /* !_WIN32 */

int CALLBACK LibMain( HINSTANCE hInst, WORD wDataSeg,
                      WORD cbHeapSize, LPSTR lpszCmdLine )
{
    return TRUE;
}

BOOL CALLBACK __loadds WEP(BOOL fSystemExit)
{
    return TRUE;
}

#endif /* !_WIN32 */
#endif /* XP_WIN */
