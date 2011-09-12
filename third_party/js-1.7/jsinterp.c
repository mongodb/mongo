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
 * JavaScript bytecode interpreter.
 */
#include "jsstddef.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "jstypes.h"
#include "jsarena.h" /* Added by JSIFY */
#include "jsutil.h" /* Added by JSIFY */
#include "jsprf.h"
#include "jsapi.h"
#include "jsarray.h"
#include "jsatom.h"
#include "jsbool.h"
#include "jscntxt.h"
#include "jsconfig.h"
#include "jsdbgapi.h"
#include "jsfun.h"
#include "jsgc.h"
#include "jsinterp.h"
#include "jsiter.h"
#include "jslock.h"
#include "jsnum.h"
#include "jsobj.h"
#include "jsopcode.h"
#include "jsscan.h"
#include "jsscope.h"
#include "jsscript.h"
#include "jsstr.h"

#if JS_HAS_XML_SUPPORT
#include "jsxml.h"
#endif

#ifdef DEBUG
#define ASSERT_CACHE_IS_EMPTY(cache)                                          \
    JS_BEGIN_MACRO                                                            \
        JSPropertyCacheEntry *end_, *pce_, entry_;                            \
        JSPropertyCache *cache_ = (cache);                                    \
        JS_ASSERT(cache_->empty);                                             \
        end_ = &cache_->table[PROPERTY_CACHE_SIZE];                           \
        for (pce_ = &cache_->table[0]; pce_ < end_; pce_++) {                 \
            PCE_LOAD(cache_, pce_, entry_);                                   \
            JS_ASSERT(!PCE_OBJECT(entry_));                                   \
            JS_ASSERT(!PCE_PROPERTY(entry_));                                 \
        }                                                                     \
    JS_END_MACRO
#else
#define ASSERT_CACHE_IS_EMPTY(cache) ((void)0)
#endif

void
js_FlushPropertyCache(JSContext *cx)
{
    JSPropertyCache *cache;

    cache = &cx->runtime->propertyCache;
    if (cache->empty) {
        ASSERT_CACHE_IS_EMPTY(cache);
        return;
    }
    memset(cache->table, 0, sizeof cache->table);
    cache->empty = JS_TRUE;
#ifdef JS_PROPERTY_CACHE_METERING
    cache->flushes++;
#endif
}

void
js_DisablePropertyCache(JSContext *cx)
{
    JS_ASSERT(!cx->runtime->propertyCache.disabled);
    cx->runtime->propertyCache.disabled = JS_TRUE;
}

void
js_EnablePropertyCache(JSContext *cx)
{
    JS_ASSERT(cx->runtime->propertyCache.disabled);
    ASSERT_CACHE_IS_EMPTY(&cx->runtime->propertyCache);
    cx->runtime->propertyCache.disabled = JS_FALSE;
}

/*
 * Stack macros and functions.  These all use a local variable, jsval *sp, to
 * point to the next free stack slot.  SAVE_SP must be called before any call
 * to a function that may invoke the interpreter.  RESTORE_SP must be called
 * only after return from js_Invoke, because only js_Invoke changes fp->sp.
 */
#define PUSH(v)         (*sp++ = (v))
#define POP()           (*--sp)
#ifdef DEBUG
#define SAVE_SP(fp)                                                           \
    (JS_ASSERT((fp)->script || !(fp)->spbase || (sp) == (fp)->spbase),        \
     (fp)->sp = sp)
#else
#define SAVE_SP(fp)     ((fp)->sp = sp)
#endif
#define RESTORE_SP(fp)  (sp = (fp)->sp)

/*
 * SAVE_SP_AND_PC commits deferred stores of interpreter registers to their
 * homes in fp, when calling out of the interpreter loop or threaded code.
 * RESTORE_SP_AND_PC copies the other way, to update registers after a call
 * to a subroutine that interprets a piece of the current script.
 */
#define SAVE_SP_AND_PC(fp)      (SAVE_SP(fp), (fp)->pc = pc)
#define RESTORE_SP_AND_PC(fp)   (RESTORE_SP(fp), pc = (fp)->pc)

/*
 * Push the generating bytecode's pc onto the parallel pc stack that runs
 * depth slots below the operands.
 *
 * NB: PUSH_OPND uses sp, depth, and pc from its lexical environment.  See
 * js_Interpret for these local variables' declarations and uses.
 */
#define PUSH_OPND(v)    (sp[-depth] = (jsval)pc, PUSH(v))
#define STORE_OPND(n,v) (sp[(n)-depth] = (jsval)pc, sp[n] = (v))
#define POP_OPND()      POP()
#define FETCH_OPND(n)   (sp[n])

/*
 * Push the jsdouble d using sp, depth, and pc from the lexical environment.
 * Try to convert d to a jsint that fits in a jsval, otherwise GC-alloc space
 * for it and push a reference.
 */
#define STORE_NUMBER(cx, n, d)                                                \
    JS_BEGIN_MACRO                                                            \
        jsint i_;                                                             \
        jsval v_;                                                             \
                                                                              \
        if (JSDOUBLE_IS_INT(d, i_) && INT_FITS_IN_JSVAL(i_)) {                \
            v_ = INT_TO_JSVAL(i_);                                            \
        } else {                                                              \
            ok = js_NewDoubleValue(cx, d, &v_);                               \
            if (!ok)                                                          \
                goto out;                                                     \
        }                                                                     \
        STORE_OPND(n, v_);                                                    \
    JS_END_MACRO

#define STORE_INT(cx, n, i)                                                   \
    JS_BEGIN_MACRO                                                            \
        jsval v_;                                                             \
                                                                              \
        if (INT_FITS_IN_JSVAL(i)) {                                           \
            v_ = INT_TO_JSVAL(i);                                             \
        } else {                                                              \
            ok = js_NewDoubleValue(cx, (jsdouble)(i), &v_);                   \
            if (!ok)                                                          \
                goto out;                                                     \
        }                                                                     \
        STORE_OPND(n, v_);                                                    \
    JS_END_MACRO

#define STORE_UINT(cx, n, u)                                                  \
    JS_BEGIN_MACRO                                                            \
        jsval v_;                                                             \
                                                                              \
        if ((u) <= JSVAL_INT_MAX) {                                           \
            v_ = INT_TO_JSVAL(u);                                             \
        } else {                                                              \
            ok = js_NewDoubleValue(cx, (jsdouble)(u), &v_);                   \
            if (!ok)                                                          \
                goto out;                                                     \
        }                                                                     \
        STORE_OPND(n, v_);                                                    \
    JS_END_MACRO

#define FETCH_NUMBER(cx, n, d)                                                \
    JS_BEGIN_MACRO                                                            \
        jsval v_;                                                             \
                                                                              \
        v_ = FETCH_OPND(n);                                                   \
        VALUE_TO_NUMBER(cx, v_, d);                                           \
    JS_END_MACRO

#define FETCH_INT(cx, n, i)                                                   \
    JS_BEGIN_MACRO                                                            \
        jsval v_ = FETCH_OPND(n);                                             \
        if (JSVAL_IS_INT(v_)) {                                               \
            i = JSVAL_TO_INT(v_);                                             \
        } else {                                                              \
            SAVE_SP_AND_PC(fp);                                               \
            ok = js_ValueToECMAInt32(cx, v_, &i);                             \
            if (!ok)                                                          \
                goto out;                                                     \
        }                                                                     \
    JS_END_MACRO

#define FETCH_UINT(cx, n, ui)                                                 \
    JS_BEGIN_MACRO                                                            \
        jsval v_ = FETCH_OPND(n);                                             \
        jsint i_;                                                             \
        if (JSVAL_IS_INT(v_) && (i_ = JSVAL_TO_INT(v_)) >= 0) {               \
            ui = (uint32) i_;                                                 \
        } else {                                                              \
            SAVE_SP_AND_PC(fp);                                               \
            ok = js_ValueToECMAUint32(cx, v_, &ui);                           \
            if (!ok)                                                          \
                goto out;                                                     \
        }                                                                     \
    JS_END_MACRO

/*
 * Optimized conversion macros that test for the desired type in v before
 * homing sp and calling a conversion function.
 */
#define VALUE_TO_NUMBER(cx, v, d)                                             \
    JS_BEGIN_MACRO                                                            \
        if (JSVAL_IS_INT(v)) {                                                \
            d = (jsdouble)JSVAL_TO_INT(v);                                    \
        } else if (JSVAL_IS_DOUBLE(v)) {                                      \
            d = *JSVAL_TO_DOUBLE(v);                                          \
        } else {                                                              \
            SAVE_SP_AND_PC(fp);                                               \
            ok = js_ValueToNumber(cx, v, &d);                                 \
            if (!ok)                                                          \
                goto out;                                                     \
        }                                                                     \
    JS_END_MACRO

#define POP_BOOLEAN(cx, v, b)                                                 \
    JS_BEGIN_MACRO                                                            \
        v = FETCH_OPND(-1);                                                   \
        if (v == JSVAL_NULL) {                                                \
            b = JS_FALSE;                                                     \
        } else if (JSVAL_IS_BOOLEAN(v)) {                                     \
            b = JSVAL_TO_BOOLEAN(v);                                          \
        } else {                                                              \
            SAVE_SP_AND_PC(fp);                                               \
            ok = js_ValueToBoolean(cx, v, &b);                                \
            if (!ok)                                                          \
                goto out;                                                     \
        }                                                                     \
        sp--;                                                                 \
    JS_END_MACRO

/*
 * Convert a primitive string, number or boolean to a corresponding object.
 * v must not be an object, null or undefined when using this macro.
 */
#define PRIMITIVE_TO_OBJECT(cx, v, obj)                                       \
    JS_BEGIN_MACRO                                                            \
        SAVE_SP(fp);                                                          \
        if (JSVAL_IS_STRING(v)) {                                             \
            obj = js_StringToObject(cx, JSVAL_TO_STRING(v));                  \
        } else if (JSVAL_IS_INT(v)) {                                         \
            obj = js_NumberToObject(cx, (jsdouble)JSVAL_TO_INT(v));           \
        } else if (JSVAL_IS_DOUBLE(v)) {                                      \
            obj = js_NumberToObject(cx, *JSVAL_TO_DOUBLE(v));                 \
        } else {                                                              \
            JS_ASSERT(JSVAL_IS_BOOLEAN(v));                                   \
            obj = js_BooleanToObject(cx, JSVAL_TO_BOOLEAN(v));                \
        }                                                                     \
    JS_END_MACRO

#define VALUE_TO_OBJECT(cx, v, obj)                                           \
    JS_BEGIN_MACRO                                                            \
        if (!JSVAL_IS_PRIMITIVE(v)) {                                         \
            obj = JSVAL_TO_OBJECT(v);                                         \
        } else {                                                              \
            SAVE_SP_AND_PC(fp);                                               \
            obj = js_ValueToNonNullObject(cx, v);                             \
            if (!obj) {                                                       \
                ok = JS_FALSE;                                                \
                goto out;                                                     \
            }                                                                 \
        }                                                                     \
    JS_END_MACRO

#define FETCH_OBJECT(cx, n, v, obj)                                           \
    JS_BEGIN_MACRO                                                            \
        v = FETCH_OPND(n);                                                    \
        VALUE_TO_OBJECT(cx, v, obj);                                          \
        STORE_OPND(n, OBJECT_TO_JSVAL(obj));                                  \
    JS_END_MACRO

#define VALUE_TO_PRIMITIVE(cx, v, hint, vp)                                   \
    JS_BEGIN_MACRO                                                            \
        if (JSVAL_IS_PRIMITIVE(v)) {                                          \
            *vp = v;                                                          \
        } else {                                                              \
            SAVE_SP_AND_PC(fp);                                               \
            ok = OBJ_DEFAULT_VALUE(cx, JSVAL_TO_OBJECT(v), hint, vp);         \
            if (!ok)                                                          \
                goto out;                                                     \
        }                                                                     \
    JS_END_MACRO

JS_FRIEND_API(jsval *)
js_AllocRawStack(JSContext *cx, uintN nslots, void **markp)
{
    jsval *sp;

    if (markp)
        *markp = JS_ARENA_MARK(&cx->stackPool);
    JS_ARENA_ALLOCATE_CAST(sp, jsval *, &cx->stackPool, nslots * sizeof(jsval));
    if (!sp) {
        JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_STACK_OVERFLOW,
                             (cx->fp && cx->fp->fun)
                             ? JS_GetFunctionName(cx->fp->fun)
                             : "script");
    }
    return sp;
}

JS_FRIEND_API(void)
js_FreeRawStack(JSContext *cx, void *mark)
{
    JS_ARENA_RELEASE(&cx->stackPool, mark);
}

JS_FRIEND_API(jsval *)
js_AllocStack(JSContext *cx, uintN nslots, void **markp)
{
    jsval *sp, *vp, *end;
    JSArena *a;
    JSStackHeader *sh;
    JSStackFrame *fp;

    /* Callers don't check for zero nslots: we do to avoid empty segments. */
    if (nslots == 0) {
        *markp = NULL;
        return JS_ARENA_MARK(&cx->stackPool);
    }

    /* Allocate 2 extra slots for the stack segment header we'll likely need. */
    sp = js_AllocRawStack(cx, 2 + nslots, markp);
    if (!sp)
        return NULL;

    /* Try to avoid another header if we can piggyback on the last segment. */
    a = cx->stackPool.current;
    sh = cx->stackHeaders;
    if (sh && JS_STACK_SEGMENT(sh) + sh->nslots == sp) {
        /* Extend the last stack segment, give back the 2 header slots. */
        sh->nslots += nslots;
        a->avail -= 2 * sizeof(jsval);
    } else {
        /*
         * Need a new stack segment, so we must initialize unused slots in the
         * current frame.  See js_GC, just before marking the "operand" jsvals,
         * where we scan from fp->spbase to fp->sp or through fp->script->depth
         * (whichever covers fewer slots).
         */
        fp = cx->fp;
        if (fp && fp->script && fp->spbase) {
#ifdef DEBUG
            jsuword depthdiff = fp->script->depth * sizeof(jsval);
            JS_ASSERT(JS_UPTRDIFF(fp->sp, fp->spbase) <= depthdiff);
            JS_ASSERT(JS_UPTRDIFF(*markp, fp->spbase) >= depthdiff);
#endif
            end = fp->spbase + fp->script->depth;
            for (vp = fp->sp; vp < end; vp++)
                *vp = JSVAL_VOID;
        }

        /* Allocate and push a stack segment header from the 2 extra slots. */
        sh = (JSStackHeader *)sp;
        sh->nslots = nslots;
        sh->down = cx->stackHeaders;
        cx->stackHeaders = sh;
        sp += 2;
    }

    /*
     * Store JSVAL_NULL using memset, to let compilers optimize as they see
     * fit, in case a caller allocates and pushes GC-things one by one, which
     * could nest a last-ditch GC that will scan this segment.
     */
    memset(sp, 0, nslots * sizeof(jsval));
    return sp;
}

JS_FRIEND_API(void)
js_FreeStack(JSContext *cx, void *mark)
{
    JSStackHeader *sh;
    jsuword slotdiff;

    /* Check for zero nslots allocation special case. */
    if (!mark)
        return;

    /* We can assert because js_FreeStack always balances js_AllocStack. */
    sh = cx->stackHeaders;
    JS_ASSERT(sh);

    /* If mark is in the current segment, reduce sh->nslots, else pop sh. */
    slotdiff = JS_UPTRDIFF(mark, JS_STACK_SEGMENT(sh)) / sizeof(jsval);
    if (slotdiff < (jsuword)sh->nslots)
        sh->nslots = slotdiff;
    else
        cx->stackHeaders = sh->down;

    /* Release the stackPool space allocated since mark was set. */
    JS_ARENA_RELEASE(&cx->stackPool, mark);
}

JSBool
js_GetArgument(JSContext *cx, JSObject *obj, jsval id, jsval *vp)
{
    return JS_TRUE;
}

JSBool
js_SetArgument(JSContext *cx, JSObject *obj, jsval id, jsval *vp)
{
    return JS_TRUE;
}

JSBool
js_GetLocalVariable(JSContext *cx, JSObject *obj, jsval id, jsval *vp)
{
    return JS_TRUE;
}

JSBool
js_SetLocalVariable(JSContext *cx, JSObject *obj, jsval id, jsval *vp)
{
    return JS_TRUE;
}

JSObject *
js_GetScopeChain(JSContext *cx, JSStackFrame *fp)
{
    JSObject *obj, *cursor, *clonedChild, *parent;
    JSTempValueRooter tvr;

    obj = fp->blockChain;
    if (!obj) {
        /*
         * Don't force a call object for a lightweight function call, but do
         * insist that there is a call object for a heavyweight function call.
         */
        JS_ASSERT(!fp->fun ||
                  !(fp->fun->flags & JSFUN_HEAVYWEIGHT) ||
                  fp->callobj);
        JS_ASSERT(fp->scopeChain);
        return fp->scopeChain;
    }

    /*
     * We have one or more lexical scopes to reflect into fp->scopeChain, so
     * make sure there's a call object at the current head of the scope chain,
     * if this frame is a call frame.
     */
    if (fp->fun && !fp->callobj) {
        JS_ASSERT(OBJ_GET_CLASS(cx, fp->scopeChain) != &js_BlockClass ||
                  JS_GetPrivate(cx, fp->scopeChain) != fp);
        if (!js_GetCallObject(cx, fp, fp->scopeChain))
            return NULL;
    }

    /*
     * Clone the block chain. To avoid recursive cloning we set the parent of
     * the cloned child after we clone the parent. In the following loop when
     * clonedChild is null it indicates the first iteration when no special GC
     * rooting is necessary. On the second and the following iterations we
     * have to protect cloned so far chain against the GC during cloning of
     * the cursor object.
     */
    cursor = obj;
    clonedChild = NULL;
    for (;;) {
        parent = OBJ_GET_PARENT(cx, cursor);

        /*
         * We pass fp->scopeChain and not null even if we override the parent
         * slot later as null triggers useless calculations of slot's value in
         * js_NewObject that js_CloneBlockObject calls.
         */
        cursor = js_CloneBlockObject(cx, cursor, fp->scopeChain, fp);
        if (!cursor) {
            if (clonedChild)
                JS_POP_TEMP_ROOT(cx, &tvr);
            return NULL;
        }
        if (!clonedChild) {
            /*
             * The first iteration. Check if other follow and root obj if so
             * to protect the whole cloned chain against GC.
             */
            obj = cursor;
            if (!parent)
                break;
            JS_PUSH_TEMP_ROOT_OBJECT(cx, obj, &tvr);
        } else {
            /*
             * Avoid OBJ_SET_PARENT overhead as clonedChild cannot escape to
             * other threads.
             */
            clonedChild->slots[JSSLOT_PARENT] = OBJECT_TO_JSVAL(cursor);
            if (!parent) {
                JS_ASSERT(tvr.u.value == OBJECT_TO_JSVAL(obj));
                JS_POP_TEMP_ROOT(cx, &tvr);
                break;
            }
        }
        clonedChild = cursor;
        cursor = parent;
    }
    fp->flags |= JSFRAME_POP_BLOCKS;
    fp->scopeChain = obj;
    fp->blockChain = NULL;
    return obj;
}

/*
 * Walk the scope chain looking for block scopes whose locals need to be
 * copied from stack slots into object slots before fp goes away.
 */
static JSBool
PutBlockObjects(JSContext *cx, JSStackFrame *fp)
{
    JSBool ok;
    JSObject *obj;

    ok = JS_TRUE;
    for (obj = fp->scopeChain; obj; obj = OBJ_GET_PARENT(cx, obj)) {
        if (OBJ_GET_CLASS(cx, obj) == &js_BlockClass) {
            if (JS_GetPrivate(cx, obj) != fp)
                break;
            ok &= js_PutBlockObject(cx, obj);
        }
    }
    return ok;
}

JSObject *
js_ComputeThis(JSContext *cx, JSObject *thisp, jsval *argv)
{
    if (thisp && OBJ_GET_CLASS(cx, thisp) != &js_CallClass) {
        /* Some objects (e.g., With) delegate 'this' to another object. */
        thisp = OBJ_THIS_OBJECT(cx, thisp);
        if (!thisp)
            return NULL;
    } else {
        /*
         * ECMA requires "the global object", but in the presence of multiple
         * top-level objects (windows, frames, or certain layers in the client
         * object model), we prefer fun's parent.  An example that causes this
         * code to run:
         *
         *   // in window w1
         *   function f() { return this }
         *   function g() { return f }
         *
         *   // in window w2
         *   var h = w1.g()
         *   alert(h() == w1)
         *
         * The alert should display "true".
         */
        if (JSVAL_IS_PRIMITIVE(argv[-2]) ||
            !OBJ_GET_PARENT(cx, JSVAL_TO_OBJECT(argv[-2]))) {
            thisp = cx->globalObject;
        } else {
            jsid id;
            jsval v;
            uintN attrs;

            /* Walk up the parent chain. */
            thisp = JSVAL_TO_OBJECT(argv[-2]);
            id = ATOM_TO_JSID(cx->runtime->atomState.parentAtom);
            for (;;) {
                if (!OBJ_CHECK_ACCESS(cx, thisp, id, JSACC_PARENT, &v, &attrs))
                    return NULL;
                if (JSVAL_IS_VOID(v))
                    v = OBJ_GET_SLOT(cx, thisp, JSSLOT_PARENT);
                if (JSVAL_IS_NULL(v))
                    break;
                thisp = JSVAL_TO_OBJECT(v);
            }
        }
    }
    argv[-1] = OBJECT_TO_JSVAL(thisp);
    return thisp;
}

#if JS_HAS_NO_SUCH_METHOD

static JSBool
NoSuchMethod(JSContext *cx, JSStackFrame *fp, jsval *vp, uint32 flags,
             uintN argc)
{
    JSObject *thisp, *argsobj;
    jsval *sp, roots[3];
    JSTempValueRooter tvr;
    jsid id;
    JSBool ok;
    jsbytecode *pc;
    jsatomid atomIndex;

    /*
     * We must call js_ComputeThis here to censor Call objects.  A performance
     * hit, since we'll call it again in the normal sequence of invoke events,
     * but at least it's idempotent.
     *
     * Normally, we call ComputeThis after all frame members have been set,
     * and in particular, after any revision of the callee value at *vp  due
     * to clasp->convert (see below).  This matters because ComputeThis may
     * access *vp via fp->argv[-2], to follow the parent chain to a global
     * object to use as the 'this' parameter.
     *
     * Obviously, here in the JSVAL_IS_PRIMITIVE(v) case, there can't be any
     * such defaulting of 'this' to callee (v, *vp) ancestor.
     */
    JS_ASSERT(JSVAL_IS_PRIMITIVE(vp[0]));
    RESTORE_SP(fp);
    if (JSVAL_IS_OBJECT(vp[1])) {
        thisp = JSVAL_TO_OBJECT(vp[1]);
    } else {
        PRIMITIVE_TO_OBJECT(cx, vp[1], thisp);
        if (!thisp)
            return JS_FALSE;
        vp[1] = OBJECT_TO_JSVAL(thisp);
    }
    thisp = js_ComputeThis(cx, thisp, vp + 2);
    if (!thisp)
        return JS_FALSE;
    vp[1] = OBJECT_TO_JSVAL(thisp);

    /* From here on, control must flow through label out: to return. */
    memset(roots, 0, sizeof roots);
    JS_PUSH_TEMP_ROOT(cx, JS_ARRAY_LENGTH(roots), roots, &tvr);

    id = ATOM_TO_JSID(cx->runtime->atomState.noSuchMethodAtom);
#if JS_HAS_XML_SUPPORT
    if (OBJECT_IS_XML(cx, thisp)) {
        JSXMLObjectOps *ops;

        ops = (JSXMLObjectOps *) thisp->map->ops;
        thisp = ops->getMethod(cx, thisp, id, &roots[2]);
        if (!thisp) {
            ok = JS_FALSE;
            goto out;
        }
        vp[1] = OBJECT_TO_JSVAL(thisp);
    } else
#endif
    {
        ok = OBJ_GET_PROPERTY(cx, thisp, id, &roots[2]);
        if (!ok)
            goto out;
    }
    if (JSVAL_IS_PRIMITIVE(roots[2]))
        goto not_function;

    pc = (jsbytecode *) vp[-(intN)fp->script->depth];
    switch ((JSOp) *pc) {
      case JSOP_NAME:
      case JSOP_GETPROP:
#if JS_HAS_XML_SUPPORT
      case JSOP_GETMETHOD:
#endif
        atomIndex = GET_ATOM_INDEX(pc);
        roots[0] = ATOM_KEY(js_GetAtom(cx, &fp->script->atomMap, atomIndex));
        argsobj = js_NewArrayObject(cx, argc, vp + 2);
        if (!argsobj) {
            ok = JS_FALSE;
            goto out;
        }
        roots[1] = OBJECT_TO_JSVAL(argsobj);
        ok = js_InternalInvoke(cx, thisp, roots[2], flags | JSINVOKE_INTERNAL,
                               2, roots, &vp[0]);
        break;

      default:
        goto not_function;
    }

  out:
    JS_POP_TEMP_ROOT(cx, &tvr);
    return ok;

  not_function:
    js_ReportIsNotFunction(cx, vp, flags & JSINVOKE_FUNFLAGS);
    ok = JS_FALSE;
    goto out;
}

#endif /* JS_HAS_NO_SUCH_METHOD */

#ifdef DUMP_CALL_TABLE

#include "jsclist.h"
#include "jshash.h"
#include "jsdtoa.h"

typedef struct CallKey {
    jsval               callee;                 /* callee value */
    const char          *filename;              /* function filename or null */
    uintN               lineno;                 /* function lineno or 0 */
} CallKey;

/* Compensate for typeof null == "object" brain damage. */
#define JSTYPE_NULL     JSTYPE_LIMIT
#define TYPEOF(cx,v)    (JSVAL_IS_NULL(v) ? JSTYPE_NULL : JS_TypeOfValue(cx,v))
#define TYPENAME(t)     (((t) == JSTYPE_NULL) ? js_null_str : js_type_str[t])
#define NTYPEHIST       (JSTYPE_LIMIT + 1)

typedef struct CallValue {
    uint32              total;                  /* total call count */
    uint32              recycled;               /* LRU-recycled calls lost */
    uint16              minargc;                /* minimum argument count */
    uint16              maxargc;                /* maximum argument count */
    struct ArgInfo {
        uint32          typeHist[NTYPEHIST];    /* histogram by type */
        JSCList         lruList;                /* top 10 values LRU list */
        struct ArgValCount {
            JSCList     lruLink;                /* LRU list linkage */
            jsval       value;                  /* recently passed value */
            uint32      count;                  /* number of times passed */
            char        strbuf[112];            /* string conversion buffer */
        } topValCounts[10];                     /* top 10 value storage */
    } argInfo[8];
} CallValue;

typedef struct CallEntry {
    JSHashEntry         entry;
    CallKey             key;
    CallValue           value;
    char                name[32];               /* function name copy */
} CallEntry;

static void *
AllocCallTable(void *pool, size_t size)
{
    return malloc(size);
}

static void
FreeCallTable(void *pool, void *item)
{
    free(item);
}

static JSHashEntry *
AllocCallEntry(void *pool, const void *key)
{
    return (JSHashEntry*) calloc(1, sizeof(CallEntry));
}

static void
FreeCallEntry(void *pool, JSHashEntry *he, uintN flag)
{
    JS_ASSERT(flag == HT_FREE_ENTRY);
    free(he);
}

static JSHashAllocOps callTableAllocOps = {
    AllocCallTable, FreeCallTable,
    AllocCallEntry, FreeCallEntry
};

JS_STATIC_DLL_CALLBACK(JSHashNumber)
js_hash_call_key(const void *key)
{
    CallKey *ck = (CallKey *) key;
    JSHashNumber hash = (jsuword)ck->callee >> 3;

    if (ck->filename) {
        hash = (hash << 4) ^ JS_HashString(ck->filename);
        hash = (hash << 4) ^ ck->lineno;
    }
    return hash;
}

JS_STATIC_DLL_CALLBACK(intN)
js_compare_call_keys(const void *k1, const void *k2)
{
    CallKey *ck1 = (CallKey *)k1, *ck2 = (CallKey *)k2;

    return ck1->callee == ck2->callee &&
           ((ck1->filename && ck2->filename)
            ? strcmp(ck1->filename, ck2->filename) == 0
            : ck1->filename == ck2->filename) &&
           ck1->lineno == ck2->lineno;
}

JSHashTable *js_CallTable;
size_t      js_LogCallToSourceLimit;

JS_STATIC_DLL_CALLBACK(intN)
CallTableDumper(JSHashEntry *he, intN k, void *arg)
{
    CallEntry *ce = (CallEntry *)he;
    FILE *fp = (FILE *)arg;
    uintN argc, i, n;
    struct ArgInfo *ai;
    JSType save, type;
    JSCList *cl;
    struct ArgValCount *avc;
    jsval argval;

    if (ce->key.filename) {
        /* We're called at the end of the mark phase, so mark our filenames. */
        js_MarkScriptFilename(ce->key.filename);
        fprintf(fp, "%s:%u ", ce->key.filename, ce->key.lineno);
    } else {
        fprintf(fp, "@%p ", (void *) ce->key.callee);
    }

    if (ce->name[0])
        fprintf(fp, "name %s ", ce->name);
    fprintf(fp, "calls %lu (%lu) argc %u/%u\n",
            (unsigned long) ce->value.total,
            (unsigned long) ce->value.recycled,
            ce->value.minargc, ce->value.maxargc);

    argc = JS_MIN(ce->value.maxargc, 8);
    for (i = 0; i < argc; i++) {
        ai = &ce->value.argInfo[i];

        n = 0;
        save = -1;
        for (type = JSTYPE_VOID; type <= JSTYPE_LIMIT; type++) {
            if (ai->typeHist[type]) {
                save = type;
                ++n;
            }
        }
        if (n == 1) {
            fprintf(fp, "  arg %u type %s: %lu\n",
                    i, TYPENAME(save), (unsigned long) ai->typeHist[save]);
        } else {
            fprintf(fp, "  arg %u type histogram:\n", i);
            for (type = JSTYPE_VOID; type <= JSTYPE_LIMIT; type++) {
                fprintf(fp, "  %9s: %8lu ",
                       TYPENAME(type), (unsigned long) ai->typeHist[type]);
                for (n = (uintN) JS_HOWMANY(ai->typeHist[type], 10); n > 0; --n)
                    fputc('*', fp);
                fputc('\n', fp);
            }
        }

        fprintf(fp, "  arg %u top 10 values:\n", i);
        n = 1;
        for (cl = ai->lruList.prev; cl != &ai->lruList; cl = cl->prev) {
            avc = (struct ArgValCount *)cl;
            if (!avc->count)
                break;
            argval = avc->value;
            fprintf(fp, "  %9u: %8lu %.*s (%#lx)\n",
                    n, (unsigned long) avc->count,
                    sizeof avc->strbuf, avc->strbuf, argval);
            ++n;
        }
    }

    return HT_ENUMERATE_NEXT;
}

void
js_DumpCallTable(JSContext *cx)
{
    char name[24];
    FILE *fp;
    static uintN dumpCount;

    if (!js_CallTable)
        return;

    JS_snprintf(name, sizeof name, "/tmp/calltable.dump.%u", dumpCount & 7);
    dumpCount++;
    fp = fopen(name, "w");
    if (!fp)
        return;

    JS_HashTableEnumerateEntries(js_CallTable, CallTableDumper, fp);
    fclose(fp);
}

static void
LogCall(JSContext *cx, jsval callee, uintN argc, jsval *argv)
{
    CallKey key;
    const char *name, *cstr;
    JSFunction *fun;
    JSHashNumber keyHash;
    JSHashEntry **hep, *he;
    CallEntry *ce;
    uintN i, j;
    jsval argval;
    JSType type;
    struct ArgInfo *ai;
    struct ArgValCount *avc;
    JSString *str;

    if (!js_CallTable) {
        js_CallTable = JS_NewHashTable(1024, js_hash_call_key,
                                       js_compare_call_keys, NULL,
                                       &callTableAllocOps, NULL);
        if (!js_CallTable)
            return;
    }

    key.callee = callee;
    key.filename = NULL;
    key.lineno = 0;
    name = "";
    if (VALUE_IS_FUNCTION(cx, callee)) {
        fun = (JSFunction *) JS_GetPrivate(cx, JSVAL_TO_OBJECT(callee));
        if (fun->atom)
            name = js_AtomToPrintableString(cx, fun->atom);
        if (FUN_INTERPRETED(fun)) {
            key.filename = fun->u.i.script->filename;
            key.lineno = fun->u.i.script->lineno;
        }
    }
    keyHash = js_hash_call_key(&key);

    hep = JS_HashTableRawLookup(js_CallTable, keyHash, &key);
    he = *hep;
    if (he) {
        ce = (CallEntry *) he;
        JS_ASSERT(strncmp(ce->name, name, sizeof ce->name) == 0);
    } else {
        he = JS_HashTableRawAdd(js_CallTable, hep, keyHash, &key, NULL);
        if (!he)
            return;
        ce = (CallEntry *) he;
        ce->entry.key = &ce->key;
        ce->entry.value = &ce->value;
        ce->key = key;
        for (i = 0; i < 8; i++) {
            ai = &ce->value.argInfo[i];
            JS_INIT_CLIST(&ai->lruList);
            for (j = 0; j < 10; j++)
                JS_APPEND_LINK(&ai->topValCounts[j].lruLink, &ai->lruList);
        }
        strncpy(ce->name, name, sizeof ce->name);
    }

    ++ce->value.total;
    if (ce->value.minargc < argc)
        ce->value.minargc = argc;
    if (ce->value.maxargc < argc)
        ce->value.maxargc = argc;
    if (argc > 8)
        argc = 8;
    for (i = 0; i < argc; i++) {
        ai = &ce->value.argInfo[i];
        argval = argv[i];
        type = TYPEOF(cx, argval);
        ++ai->typeHist[type];

        for (j = 0; ; j++) {
            if (j == 10) {
                avc = (struct ArgValCount *) ai->lruList.next;
                ce->value.recycled += avc->count;
                avc->value = argval;
                avc->count = 1;
                break;
            }
            avc = &ai->topValCounts[j];
            if (avc->value == argval) {
                ++avc->count;
                break;
            }
        }

        /* Move avc to the back of the LRU list. */
        JS_REMOVE_LINK(&avc->lruLink);
        JS_APPEND_LINK(&avc->lruLink, &ai->lruList);

        str = NULL;
        cstr = "";
        switch (TYPEOF(cx, argval)) {
          case JSTYPE_VOID:
            cstr = js_type_str[JSTYPE_VOID];
            break;
          case JSTYPE_NULL:
            cstr = js_null_str;
            break;
          case JSTYPE_BOOLEAN:
            cstr = js_boolean_str[JSVAL_TO_BOOLEAN(argval)];
            break;
          case JSTYPE_NUMBER:
            if (JSVAL_IS_INT(argval)) {
                JS_snprintf(avc->strbuf, sizeof avc->strbuf, "%ld",
                            JSVAL_TO_INT(argval));
            } else {
                JS_dtostr(avc->strbuf, sizeof avc->strbuf, DTOSTR_STANDARD, 0,
                          *JSVAL_TO_DOUBLE(argval));
            }
            continue;
          case JSTYPE_STRING:
            str = js_QuoteString(cx, JSVAL_TO_STRING(argval), (jschar)'"');
            break;
          case JSTYPE_FUNCTION:
            if (VALUE_IS_FUNCTION(cx, argval)) {
                fun = (JSFunction *)JS_GetPrivate(cx, JSVAL_TO_OBJECT(argval));
                if (fun && fun->atom) {
                    str = ATOM_TO_STRING(fun->atom);
                    break;
                }
            }
            /* FALL THROUGH */
          case JSTYPE_OBJECT:
            js_LogCallToSourceLimit = sizeof avc->strbuf;
            cx->options |= JSOPTION_LOGCALL_TOSOURCE;
            str = js_ValueToSource(cx, argval);
            cx->options &= ~JSOPTION_LOGCALL_TOSOURCE;
            break;
        }
        if (str)
            cstr = JS_GetStringBytes(str);
        strncpy(avc->strbuf, cstr, sizeof avc->strbuf);
    }
}

#endif /* DUMP_CALL_TABLE */

/*
 * Conditional assert to detect failure to clear a pending exception that is
 * suppressed (or unintentional suppression of a wanted exception).
 */
#if defined DEBUG_brendan || defined DEBUG_mrbkap || defined DEBUG_shaver
# define DEBUG_NOT_THROWING 1
#endif

#ifdef DEBUG_NOT_THROWING
# define ASSERT_NOT_THROWING(cx) JS_ASSERT(!(cx)->throwing)
#else
# define ASSERT_NOT_THROWING(cx) /* nothing */
#endif

/*
 * Find a function reference and its 'this' object implicit first parameter
 * under argc arguments on cx's stack, and call the function.  Push missing
 * required arguments, allocate declared local variables, and pop everything
 * when done.  Then push the return value.
 */
JS_FRIEND_API(JSBool)
js_Invoke(JSContext *cx, uintN argc, uintN flags)
{
    void *mark;
    JSStackFrame *fp, frame;
    jsval *sp, *newsp, *limit;
    jsval *vp, v, thisv;
    JSObject *funobj, *parent, *thisp;
    JSBool ok;
    JSClass *clasp;
    JSObjectOps *ops;
    JSNative native;
    JSFunction *fun;
    JSScript *script;
    uintN nslots, nvars, nalloc, surplus;
    JSInterpreterHook hook;
    void *hookData;

    /* Mark the top of stack and load frequently-used registers. */
    mark = JS_ARENA_MARK(&cx->stackPool);
    fp = cx->fp;
    sp = fp->sp;

    /*
     * Set vp to the callee value's stack slot (it's where rval goes).
     * Once vp is set, control should flow through label out2: to return.
     * Set frame.rval early so native class and object ops can throw and
     * return false, causing a goto out2 with ok set to false.
     */
    vp = sp - (2 + argc);
    v = *vp;
    frame.rval = JSVAL_VOID;

    /*
     * A callee must be an object reference, unless its 'this' parameter
     * implements the __noSuchMethod__ method, in which case that method will
     * be called like so:
     *
     *   thisp.__noSuchMethod__(id, args)
     *
     * where id is the name of the method that this invocation attempted to
     * call by name, and args is an Array containing this invocation's actual
     * parameters.
     */
    if (JSVAL_IS_PRIMITIVE(v)) {
#if JS_HAS_NO_SUCH_METHOD
        if (fp->script && !(flags & JSINVOKE_INTERNAL)) {
            ok = NoSuchMethod(cx, fp, vp, flags, argc);
            if (ok)
                frame.rval = *vp;
            goto out2;
        }
#endif
        goto bad;
    }

    /* Load thisv after potentially calling NoSuchMethod, which may set it. */
    thisv = vp[1];

    funobj = JSVAL_TO_OBJECT(v);
    parent = OBJ_GET_PARENT(cx, funobj);
    clasp = OBJ_GET_CLASS(cx, funobj);
    if (clasp != &js_FunctionClass) {
        /* Function is inlined, all other classes use object ops. */
        ops = funobj->map->ops;

        /*
         * XXX this makes no sense -- why convert to function if clasp->call?
         * XXX better to call that hook without converting
         * XXX the only thing that needs fixing is liveconnect
         *
         * Try converting to function, for closure and API compatibility.
         * We attempt the conversion under all circumstances for 1.2, but
         * only if there is a call op defined otherwise.
         */
        if ((ops == &js_ObjectOps) ? clasp->call : ops->call) {
            ok = clasp->convert(cx, funobj, JSTYPE_FUNCTION, &v);
            if (!ok)
                goto out2;

            if (VALUE_IS_FUNCTION(cx, v)) {
                /* Make vp refer to funobj to keep it available as argv[-2]. */
                *vp = v;
                funobj = JSVAL_TO_OBJECT(v);
                parent = OBJ_GET_PARENT(cx, funobj);
                goto have_fun;
            }
        }
        fun = NULL;
        script = NULL;
        nslots = nvars = 0;

        /* Try a call or construct native object op. */
        native = (flags & JSINVOKE_CONSTRUCT) ? ops->construct : ops->call;
        if (!native)
            goto bad;

        if (JSVAL_IS_OBJECT(thisv)) {
            thisp = JSVAL_TO_OBJECT(thisv);
        } else {
            PRIMITIVE_TO_OBJECT(cx, thisv, thisp);
            if (!thisp)
                goto out2;
            vp[1] = thisv = OBJECT_TO_JSVAL(thisp);
        }
    } else {
have_fun:
        /* Get private data and set derived locals from it. */
        fun = (JSFunction *) JS_GetPrivate(cx, funobj);
        nslots = (fun->nargs > argc) ? fun->nargs - argc : 0;
        if (FUN_INTERPRETED(fun)) {
            native = NULL;
            script = fun->u.i.script;
            nvars = fun->u.i.nvars;
        } else {
            native = fun->u.n.native;
            script = NULL;
            nvars = 0;
            nslots += fun->u.n.extra;
        }

        if (JSFUN_BOUND_METHOD_TEST(fun->flags)) {
            /* Handle bound method special case. */
            thisp = parent;
        } else if (JSVAL_IS_OBJECT(thisv)) {
            thisp = JSVAL_TO_OBJECT(thisv);
        } else {
            uintN thispflags = JSFUN_THISP_FLAGS(fun->flags);

            JS_ASSERT(!(flags & JSINVOKE_CONSTRUCT));
            if (JSVAL_IS_STRING(thisv)) {
                if (JSFUN_THISP_TEST(thispflags, JSFUN_THISP_STRING)) {
                    thisp = (JSObject *) thisv;
                    goto init_frame;
                }
                thisp = js_StringToObject(cx, JSVAL_TO_STRING(thisv));
            } else if (JSVAL_IS_INT(thisv)) {
                if (JSFUN_THISP_TEST(thispflags, JSFUN_THISP_NUMBER)) {
                    thisp = (JSObject *) thisv;
                    goto init_frame;
                }
                thisp = js_NumberToObject(cx, (jsdouble)JSVAL_TO_INT(thisv));
            } else if (JSVAL_IS_DOUBLE(thisv)) {
                if (JSFUN_THISP_TEST(thispflags, JSFUN_THISP_NUMBER)) {
                    thisp = (JSObject *) thisv;
                    goto init_frame;
                }
                thisp = js_NumberToObject(cx, *JSVAL_TO_DOUBLE(thisv));
            } else {
                JS_ASSERT(JSVAL_IS_BOOLEAN(thisv));
                if (JSFUN_THISP_TEST(thispflags, JSFUN_THISP_BOOLEAN)) {
                    thisp = (JSObject *) thisv;
                    goto init_frame;
                }
                thisp = js_BooleanToObject(cx, JSVAL_TO_BOOLEAN(thisv));
            }
            if (!thisp) {
                ok = JS_FALSE;
                goto out2;
            }
            goto init_frame;
        }
    }

    if (flags & JSINVOKE_CONSTRUCT) {
        /* Default return value for a constructor is the new object. */
        frame.rval = OBJECT_TO_JSVAL(thisp);
    } else {
        thisp = js_ComputeThis(cx, thisp, vp + 2);
        if (!thisp) {
            ok = JS_FALSE;
            goto out2;
        }
    }

  init_frame:
    /* Initialize the rest of frame, except for sp (set by SAVE_SP later). */
    frame.thisp = thisp;
    frame.varobj = NULL;
    frame.callobj = frame.argsobj = NULL;
    frame.script = script;
    frame.fun = fun;
    frame.argc = argc;
    frame.argv = sp - argc;
    frame.nvars = nvars;
    frame.vars = sp;
    frame.down = fp;
    frame.annotation = NULL;
    frame.scopeChain = NULL;    /* set below for real, after cx->fp is set */
    frame.pc = NULL;
    frame.spbase = NULL;
    frame.sharpDepth = 0;
    frame.sharpArray = NULL;
    frame.flags = flags;
    frame.dormantNext = NULL;
    frame.xmlNamespace = NULL;
    frame.blockChain = NULL;

    /* From here on, control must flow through label out: to return. */
    cx->fp = &frame;

    /* Init these now in case we goto out before first hook call. */
    hook = cx->runtime->callHook;
    hookData = NULL;

    /* Check for argument slots required by the function. */
    if (nslots) {
        /* All arguments must be contiguous, so we may have to copy actuals. */
        nalloc = nslots;
        limit = (jsval *) cx->stackPool.current->limit;
        JS_ASSERT((jsval *) cx->stackPool.current->base <= sp && sp <= limit);
        if (sp + nslots > limit) {
            /* Hit end of arena: we have to copy argv[-2..(argc+nslots-1)]. */
            nalloc += 2 + argc;
        } else {
            /* Take advantage of surplus slots in the caller's frame depth. */
            JS_ASSERT((jsval *)mark >= sp);
            surplus = (jsval *)mark - sp;
            nalloc -= surplus;
        }

        /* Check whether we have enough space in the caller's frame. */
        if ((intN)nalloc > 0) {
            /* Need space for actuals plus missing formals minus surplus. */
            newsp = js_AllocRawStack(cx, nalloc, NULL);
            if (!newsp) {
                ok = JS_FALSE;
                goto out;
            }

            /* If we couldn't allocate contiguous args, copy actuals now. */
            if (newsp != mark) {
                JS_ASSERT(sp + nslots > limit);
                JS_ASSERT(2 + argc + nslots == nalloc);
                *newsp++ = vp[0];
                *newsp++ = vp[1];
                if (argc)
                    memcpy(newsp, frame.argv, argc * sizeof(jsval));
                frame.argv = newsp;
                sp = frame.vars = newsp + argc;
            }
        }

        /* Advance frame.vars to make room for the missing args. */
        frame.vars += nslots;

        /* Push void to initialize missing args. */
        do {
            PUSH(JSVAL_VOID);
        } while (--nslots != 0);
    }
    JS_ASSERT(nslots == 0);

    /* Now allocate stack space for local variables. */
    if (nvars) {
        JS_ASSERT((jsval *)cx->stackPool.current->avail >= frame.vars);
        surplus = (jsval *)cx->stackPool.current->avail - frame.vars;
        if (surplus < nvars) {
            newsp = js_AllocRawStack(cx, nvars, NULL);
            if (!newsp) {
                ok = JS_FALSE;
                goto out;
            }
            if (newsp != sp) {
                /* NB: Discontinuity between argv and vars. */
                sp = frame.vars = newsp;
            }
        }

        /* Push void to initialize local variables. */
        do {
            PUSH(JSVAL_VOID);
        } while (--nvars != 0);
    }
    JS_ASSERT(nvars == 0);

    /* Store the current sp in frame before calling fun. */
    SAVE_SP(&frame);

    /* call the hook if present */
    if (hook && (native || script))
        hookData = hook(cx, &frame, JS_TRUE, 0, cx->runtime->callHookData);

    /* Call the function, either a native method or an interpreted script. */
    if (native) {
#ifdef DEBUG_NOT_THROWING
        JSBool alreadyThrowing = cx->throwing;
#endif

#if JS_HAS_LVALUE_RETURN
        /* Set by JS_SetCallReturnValue2, used to return reference types. */
        cx->rval2set = JS_FALSE;
#endif

        /* If native, use caller varobj and scopeChain for eval. */
        frame.varobj = fp->varobj;
        frame.scopeChain = fp->scopeChain;
        ok = native(cx, frame.thisp, argc, frame.argv, &frame.rval);
        JS_RUNTIME_METER(cx->runtime, nativeCalls);
#ifdef DEBUG_NOT_THROWING
        if (ok && !alreadyThrowing)
            ASSERT_NOT_THROWING(cx);
#endif
    } else if (script) {
#ifdef DUMP_CALL_TABLE
        LogCall(cx, *vp, argc, frame.argv);
#endif
        /* Use parent scope so js_GetCallObject can find the right "Call". */
        frame.scopeChain = parent;
        if (JSFUN_HEAVYWEIGHT_TEST(fun->flags)) {
            /* Scope with a call object parented by the callee's parent. */
            if (!js_GetCallObject(cx, &frame, parent)) {
                ok = JS_FALSE;
                goto out;
            }
        }
        ok = js_Interpret(cx, script->code, &v);
    } else {
        /* fun might be onerror trying to report a syntax error in itself. */
        frame.scopeChain = NULL;
        ok = JS_TRUE;
    }

out:
    if (hookData) {
        hook = cx->runtime->callHook;
        if (hook)
            hook(cx, &frame, JS_FALSE, &ok, hookData);
    }

    /* If frame has a call object, sync values and clear back-pointer. */
    if (frame.callobj)
        ok &= js_PutCallObject(cx, &frame);

    /* If frame has an arguments object, sync values and clear back-pointer. */
    if (frame.argsobj)
        ok &= js_PutArgsObject(cx, &frame);

    /* Restore cx->fp now that we're done releasing frame objects. */
    cx->fp = fp;

out2:
    /* Pop everything we may have allocated off the stack. */
    JS_ARENA_RELEASE(&cx->stackPool, mark);

    /* Store the return value and restore sp just above it. */
    *vp = frame.rval;
    fp->sp = vp + 1;

    /*
     * Store the location of the JSOP_CALL or JSOP_EVAL that generated the
     * return value, but only if this is an external (compiled from script
     * source) call that has stack budget for the generating pc.
     */
    if (fp->script && !(flags & JSINVOKE_INTERNAL))
        vp[-(intN)fp->script->depth] = (jsval)fp->pc;
    return ok;

bad:
    js_ReportIsNotFunction(cx, vp, flags & JSINVOKE_FUNFLAGS);
    ok = JS_FALSE;
    goto out2;
}

JSBool
js_InternalInvoke(JSContext *cx, JSObject *obj, jsval fval, uintN flags,
                  uintN argc, jsval *argv, jsval *rval)
{
    JSStackFrame *fp, *oldfp, frame;
    jsval *oldsp, *sp;
    void *mark;
    uintN i;
    JSBool ok;

    fp = oldfp = cx->fp;
    if (!fp) {
        memset(&frame, 0, sizeof frame);
        cx->fp = fp = &frame;
    }
    oldsp = fp->sp;
    sp = js_AllocStack(cx, 2 + argc, &mark);
    if (!sp) {
        ok = JS_FALSE;
        goto out;
    }

    PUSH(fval);
    PUSH(OBJECT_TO_JSVAL(obj));
    for (i = 0; i < argc; i++)
        PUSH(argv[i]);
    SAVE_SP(fp);
    ok = js_Invoke(cx, argc, flags | JSINVOKE_INTERNAL);
    if (ok) {
        RESTORE_SP(fp);

        /*
         * Store *rval in the a scoped local root if a scope is open, else in
         * the lastInternalResult pigeon-hole GC root, solely so users of
         * js_InternalInvoke and its direct and indirect (js_ValueToString for
         * example) callers do not need to manage roots for local, temporary
         * references to such results.
         */
        *rval = POP_OPND();
        if (JSVAL_IS_GCTHING(*rval)) {
            if (cx->localRootStack) {
                if (js_PushLocalRoot(cx, cx->localRootStack, *rval) < 0)
                    ok = JS_FALSE;
            } else {
                cx->weakRoots.lastInternalResult = *rval;
            }
        }
    }

    js_FreeStack(cx, mark);
out:
    fp->sp = oldsp;
    if (oldfp != fp)
        cx->fp = oldfp;

    return ok;
}

JSBool
js_InternalGetOrSet(JSContext *cx, JSObject *obj, jsid id, jsval fval,
                    JSAccessMode mode, uintN argc, jsval *argv, jsval *rval)
{
    int stackDummy;

    /*
     * js_InternalInvoke could result in another try to get or set the same id
     * again, see bug 355497.
     */
    if (!JS_CHECK_STACK_SIZE(cx, stackDummy)) {
        JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL,
                             JSMSG_OVER_RECURSED);
        return JS_FALSE;
    }
    /*
     * Check general (not object-ops/class-specific) access from the running
     * script to obj.id only if id has a scripted getter or setter that we're
     * about to invoke.  If we don't check this case, nothing else will -- no
     * other native code has the chance to check.
     *
     * Contrast this non-native (scripted) case with native getter and setter
     * accesses, where the native itself must do an access check, if security
     * policies requires it.  We make a checkAccess or checkObjectAccess call
     * back to the embedding program only in those cases where we're not going
     * to call an embedding-defined native function, getter, setter, or class
     * hook anyway.  Where we do call such a native, there's no need for the
     * engine to impose a separate access check callback on all embeddings --
     * many embeddings have no security policy at all.
     */
    JS_ASSERT(mode == JSACC_READ || mode == JSACC_WRITE);
    if (cx->runtime->checkObjectAccess &&
        VALUE_IS_FUNCTION(cx, fval) &&
        FUN_INTERPRETED((JSFunction *)
                        JS_GetPrivate(cx, JSVAL_TO_OBJECT(fval))) &&
        !cx->runtime->checkObjectAccess(cx, obj, ID_TO_VALUE(id), mode,
                                        &fval)) {
        return JS_FALSE;
    }

    return js_InternalCall(cx, obj, fval, argc, argv, rval);
}

JSBool
js_Execute(JSContext *cx, JSObject *chain, JSScript *script,
           JSStackFrame *down, uintN flags, jsval *result)
{
    JSInterpreterHook hook;
    void *hookData, *mark;
    JSStackFrame *oldfp, frame;
    JSObject *obj, *tmp;
    JSBool ok;

    hook = cx->runtime->executeHook;
    hookData = mark = NULL;
    oldfp = cx->fp;
    frame.script = script;
    if (down) {
        /* Propagate arg/var state for eval and the debugger API. */
        frame.callobj = down->callobj;
        frame.argsobj = down->argsobj;
        frame.varobj = down->varobj;
        frame.fun = down->fun;
        frame.thisp = down->thisp;
        frame.argc = down->argc;
        frame.argv = down->argv;
        frame.nvars = down->nvars;
        frame.vars = down->vars;
        frame.annotation = down->annotation;
        frame.sharpArray = down->sharpArray;
    } else {
        frame.callobj = frame.argsobj = NULL;
        obj = chain;
        if (cx->options & JSOPTION_VAROBJFIX) {
            while ((tmp = OBJ_GET_PARENT(cx, obj)) != NULL)
                obj = tmp;
        }
        frame.varobj = obj;
        frame.fun = NULL;
        frame.thisp = chain;
        frame.argc = 0;
        frame.argv = NULL;
        frame.nvars = script->numGlobalVars;
        if (frame.nvars) {
            frame.vars = js_AllocRawStack(cx, frame.nvars, &mark);
            if (!frame.vars)
                return JS_FALSE;
            memset(frame.vars, 0, frame.nvars * sizeof(jsval));
        } else {
            frame.vars = NULL;
        }
        frame.annotation = NULL;
        frame.sharpArray = NULL;
    }
    frame.rval = JSVAL_VOID;
    frame.down = down;
    frame.scopeChain = chain;
    frame.pc = NULL;
    frame.sp = oldfp ? oldfp->sp : NULL;
    frame.spbase = NULL;
    frame.sharpDepth = 0;
    frame.flags = flags;
    frame.dormantNext = NULL;
    frame.xmlNamespace = NULL;
    frame.blockChain = NULL;

    /*
     * Here we wrap the call to js_Interpret with code to (conditionally)
     * save and restore the old stack frame chain into a chain of 'dormant'
     * frame chains.  Since we are replacing cx->fp, we were running into
     * the problem that if GC was called under this frame, some of the GC
     * things associated with the old frame chain (available here only in
     * the C variable 'oldfp') were not rooted and were being collected.
     *
     * So, now we preserve the links to these 'dormant' frame chains in cx
     * before calling js_Interpret and cleanup afterwards.  The GC walks
     * these dormant chains and marks objects in the same way that it marks
     * objects in the primary cx->fp chain.
     */
    if (oldfp && oldfp != down) {
        JS_ASSERT(!oldfp->dormantNext);
        oldfp->dormantNext = cx->dormantFrameChain;
        cx->dormantFrameChain = oldfp;
    }

    cx->fp = &frame;
    if (hook)
        hookData = hook(cx, &frame, JS_TRUE, 0, cx->runtime->executeHookData);

    /*
     * Use frame.rval, not result, so the last result stays rooted across any
     * GC activations nested within this js_Interpret.
     */
    ok = js_Interpret(cx, script->code, &frame.rval);
    *result = frame.rval;

    if (hookData) {
        hook = cx->runtime->executeHook;
        if (hook)
            hook(cx, &frame, JS_FALSE, &ok, hookData);
    }
    if (mark)
        js_FreeRawStack(cx, mark);
    cx->fp = oldfp;

    if (oldfp && oldfp != down) {
        JS_ASSERT(cx->dormantFrameChain == oldfp);
        cx->dormantFrameChain = oldfp->dormantNext;
        oldfp->dormantNext = NULL;
    }

    return ok;
}

#if JS_HAS_EXPORT_IMPORT
/*
 * If id is JSVAL_VOID, import all exported properties from obj.
 */
static JSBool
ImportProperty(JSContext *cx, JSObject *obj, jsid id)
{
    JSBool ok;
    JSIdArray *ida;
    JSProperty *prop;
    JSObject *obj2, *target, *funobj, *closure;
    JSString *str;
    uintN attrs;
    jsint i;
    jsval value;

    if (JSVAL_IS_VOID(id)) {
        ida = JS_Enumerate(cx, obj);
        if (!ida)
            return JS_FALSE;
        ok = JS_TRUE;
        if (ida->length == 0)
            goto out;
    } else {
        ida = NULL;
        if (!OBJ_LOOKUP_PROPERTY(cx, obj, id, &obj2, &prop))
            return JS_FALSE;
        if (!prop) {
            str = js_DecompileValueGenerator(cx, JSDVG_IGNORE_STACK,
                                             ID_TO_VALUE(id), NULL);
            if (str)
                js_ReportIsNotDefined(cx, JS_GetStringBytes(str));
            return JS_FALSE;
        }
        ok = OBJ_GET_ATTRIBUTES(cx, obj, id, prop, &attrs);
        OBJ_DROP_PROPERTY(cx, obj2, prop);
        if (!ok)
            return JS_FALSE;
        if (!(attrs & JSPROP_EXPORTED)) {
            str = js_DecompileValueGenerator(cx, JSDVG_IGNORE_STACK,
                                             ID_TO_VALUE(id), NULL);
            if (str) {
                JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL,
                                     JSMSG_NOT_EXPORTED,
                                     JS_GetStringBytes(str));
            }
            return JS_FALSE;
        }
    }

    target = cx->fp->varobj;
    i = 0;
    do {
        if (ida) {
            id = ida->vector[i];
            ok = OBJ_GET_ATTRIBUTES(cx, obj, id, NULL, &attrs);
            if (!ok)
                goto out;
            if (!(attrs & JSPROP_EXPORTED))
                continue;
        }
        ok = OBJ_CHECK_ACCESS(cx, obj, id, JSACC_IMPORT, &value, &attrs);
        if (!ok)
            goto out;
        if (VALUE_IS_FUNCTION(cx, value)) {
            funobj = JSVAL_TO_OBJECT(value);
            closure = js_CloneFunctionObject(cx, funobj, obj);
            if (!closure) {
                ok = JS_FALSE;
                goto out;
            }
            value = OBJECT_TO_JSVAL(closure);
        }

        /*
         * Handle the case of importing a property that refers to a local
         * variable or formal parameter of a function activation.  These
         * properties are accessed by opcodes using stack slot numbers
         * generated by the compiler rather than runtime name-lookup.  These
         * local references, therefore, bypass the normal scope chain lookup.
         * So, instead of defining a new property in the activation object,
         * modify the existing value in the stack slot.
         */
        if (OBJ_GET_CLASS(cx, target) == &js_CallClass) {
            ok = OBJ_LOOKUP_PROPERTY(cx, target, id, &obj2, &prop);
            if (!ok)
                goto out;
        } else {
            prop = NULL;
        }
        if (prop && target == obj2) {
            ok = OBJ_SET_PROPERTY(cx, target, id, &value);
        } else {
            ok = OBJ_DEFINE_PROPERTY(cx, target, id, value, NULL, NULL,
                                     attrs & ~(JSPROP_EXPORTED |
                                               JSPROP_GETTER |
                                               JSPROP_SETTER),
                                     NULL);
        }
        if (prop)
            OBJ_DROP_PROPERTY(cx, obj2, prop);
        if (!ok)
            goto out;
    } while (ida && ++i < ida->length);

out:
    if (ida)
        JS_DestroyIdArray(cx, ida);
    return ok;
}
#endif /* JS_HAS_EXPORT_IMPORT */

JSBool
js_CheckRedeclaration(JSContext *cx, JSObject *obj, jsid id, uintN attrs,
                      JSObject **objp, JSProperty **propp)
{
    JSObject *obj2;
    JSProperty *prop;
    uintN oldAttrs, report;
    JSBool isFunction;
    jsval value;
    const char *type, *name;

    if (!OBJ_LOOKUP_PROPERTY(cx, obj, id, &obj2, &prop))
        return JS_FALSE;
    if (propp) {
        *objp = obj2;
        *propp = prop;
    }
    if (!prop)
        return JS_TRUE;

    /*
     * Use prop as a speedup hint to OBJ_GET_ATTRIBUTES, but drop it on error.
     * An assertion at label bad: will insist that it is null.
     */
    if (!OBJ_GET_ATTRIBUTES(cx, obj2, id, prop, &oldAttrs)) {
        OBJ_DROP_PROPERTY(cx, obj2, prop);
#ifdef DEBUG
        prop = NULL;
#endif
        goto bad;
    }

    /*
     * From here, return true, or else goto bad on failure to null out params.
     * If our caller doesn't want prop, drop it (we don't need it any longer).
     */
    if (!propp) {
        OBJ_DROP_PROPERTY(cx, obj2, prop);
        prop = NULL;
    }

    /* If either property is readonly, we have an error. */
    report = ((oldAttrs | attrs) & JSPROP_READONLY)
             ? JSREPORT_ERROR
             : JSREPORT_WARNING | JSREPORT_STRICT;

    if (report != JSREPORT_ERROR) {
        /*
         * Allow redeclaration of variables and functions, but insist that the
         * new value is not a getter if the old value was, ditto for setters --
         * unless prop is impermanent (in which case anyone could delete it and
         * redefine it, willy-nilly).
         */
        if (!(attrs & (JSPROP_GETTER | JSPROP_SETTER)))
            return JS_TRUE;
        if ((~(oldAttrs ^ attrs) & (JSPROP_GETTER | JSPROP_SETTER)) == 0)
            return JS_TRUE;
        if (!(oldAttrs & JSPROP_PERMANENT))
            return JS_TRUE;
        report = JSREPORT_ERROR;
    }

    isFunction = (oldAttrs & (JSPROP_GETTER | JSPROP_SETTER)) != 0;
    if (!isFunction) {
        if (!OBJ_GET_PROPERTY(cx, obj, id, &value))
            goto bad;
        isFunction = VALUE_IS_FUNCTION(cx, value);
    }
    type = (oldAttrs & attrs & JSPROP_GETTER)
           ? js_getter_str
           : (oldAttrs & attrs & JSPROP_SETTER)
           ? js_setter_str
           : (oldAttrs & JSPROP_READONLY)
           ? js_const_str
           : isFunction
           ? js_function_str
           : js_var_str;
    name = js_AtomToPrintableString(cx, JSID_TO_ATOM(id));
    if (!name)
        goto bad;
    return JS_ReportErrorFlagsAndNumber(cx, report,
                                        js_GetErrorMessage, NULL,
                                        JSMSG_REDECLARED_VAR,
                                        type, name);

bad:
    if (propp) {
        *objp = NULL;
        *propp = NULL;
    }
    JS_ASSERT(!prop);
    return JS_FALSE;
}

JSBool
js_StrictlyEqual(jsval lval, jsval rval)
{
    jsval ltag = JSVAL_TAG(lval), rtag = JSVAL_TAG(rval);
    jsdouble ld, rd;

    if (ltag == rtag) {
        if (ltag == JSVAL_STRING) {
            JSString *lstr = JSVAL_TO_STRING(lval),
                     *rstr = JSVAL_TO_STRING(rval);
            return js_EqualStrings(lstr, rstr);
        }
        if (ltag == JSVAL_DOUBLE) {
            ld = *JSVAL_TO_DOUBLE(lval);
            rd = *JSVAL_TO_DOUBLE(rval);
            return JSDOUBLE_COMPARE(ld, ==, rd, JS_FALSE);
        }
        return lval == rval;
    }
    if (ltag == JSVAL_DOUBLE && JSVAL_IS_INT(rval)) {
        ld = *JSVAL_TO_DOUBLE(lval);
        rd = JSVAL_TO_INT(rval);
        return JSDOUBLE_COMPARE(ld, ==, rd, JS_FALSE);
    }
    if (JSVAL_IS_INT(lval) && rtag == JSVAL_DOUBLE) {
        ld = JSVAL_TO_INT(lval);
        rd = *JSVAL_TO_DOUBLE(rval);
        return JSDOUBLE_COMPARE(ld, ==, rd, JS_FALSE);
    }
    return lval == rval;
}

JSBool
js_InvokeConstructor(JSContext *cx, jsval *vp, uintN argc)
{
    JSFunction *fun;
    JSObject *obj, *obj2, *proto, *parent;
    jsval lval, rval;
    JSClass *clasp, *funclasp;

    fun = NULL;
    obj2 = NULL;
    lval = *vp;
    if (!JSVAL_IS_OBJECT(lval) ||
        (obj2 = JSVAL_TO_OBJECT(lval)) == NULL ||
        /* XXX clean up to avoid special cases above ObjectOps layer */
        OBJ_GET_CLASS(cx, obj2) == &js_FunctionClass ||
        !obj2->map->ops->construct)
    {
        fun = js_ValueToFunction(cx, vp, JSV2F_CONSTRUCT);
        if (!fun)
            return JS_FALSE;
    }

    clasp = &js_ObjectClass;
    if (!obj2) {
        proto = parent = NULL;
        fun = NULL;
    } else {
        /*
         * Get the constructor prototype object for this function.
         * Use the nominal 'this' parameter slot, vp[1], as a local
         * root to protect this prototype, in case it has no other
         * strong refs.
         */
        if (!OBJ_GET_PROPERTY(cx, obj2,
                              ATOM_TO_JSID(cx->runtime->atomState
                                           .classPrototypeAtom),
                              &vp[1])) {
            return JS_FALSE;
        }
        rval = vp[1];
        proto = JSVAL_IS_OBJECT(rval) ? JSVAL_TO_OBJECT(rval) : NULL;
        parent = OBJ_GET_PARENT(cx, obj2);

        if (OBJ_GET_CLASS(cx, obj2) == &js_FunctionClass) {
            funclasp = ((JSFunction *)JS_GetPrivate(cx, obj2))->clasp;
            if (funclasp)
                clasp = funclasp;
        }
    }
    obj = js_NewObject(cx, clasp, proto, parent);
    if (!obj)
        return JS_FALSE;

    /* Now we have an object with a constructor method; call it. */
    vp[1] = OBJECT_TO_JSVAL(obj);
    if (!js_Invoke(cx, argc, JSINVOKE_CONSTRUCT)) {
        cx->weakRoots.newborn[GCX_OBJECT] = NULL;
        return JS_FALSE;
    }

    /* Check the return value and if it's primitive, force it to be obj. */
    rval = *vp;
    if (JSVAL_IS_PRIMITIVE(rval)) {
        if (!fun) {
            /* native [[Construct]] returning primitive is error */
            JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL,
                                 JSMSG_BAD_NEW_RESULT,
                                 js_ValueToPrintableString(cx, rval));
            return JS_FALSE;
        }
        *vp = OBJECT_TO_JSVAL(obj);
    }

    JS_RUNTIME_METER(cx->runtime, constructs);
    return JS_TRUE;
}

static JSBool
InternStringElementId(JSContext *cx, jsval idval, jsid *idp)
{
    JSAtom *atom;

    atom = js_ValueToStringAtom(cx, idval);
    if (!atom)
        return JS_FALSE;
    *idp = ATOM_TO_JSID(atom);
    return JS_TRUE;
}

static JSBool
InternNonIntElementId(JSContext *cx, jsval idval, jsid *idp)
{
    JS_ASSERT(!JSVAL_IS_INT(idval));

#if JS_HAS_XML_SUPPORT
    if (JSVAL_IS_OBJECT(idval)) {
        *idp = OBJECT_JSVAL_TO_JSID(idval);
        return JS_TRUE;
    }
#endif

    return InternStringElementId(cx, idval, idp);
}

#if JS_HAS_XML_SUPPORT
#define CHECK_ELEMENT_ID(obj, id)                                             \
    JS_BEGIN_MACRO                                                            \
        if (JSID_IS_OBJECT(id) && !OBJECT_IS_XML(cx, obj)) {                  \
            SAVE_SP_AND_PC(fp);                                               \
            ok = InternStringElementId(cx, OBJECT_JSID_TO_JSVAL(id), &id);    \
            if (!ok)                                                          \
                goto out;                                                     \
        }                                                                     \
    JS_END_MACRO

#else
#define CHECK_ELEMENT_ID(obj, id)       JS_ASSERT(!JSID_IS_OBJECT(id))
#endif

#ifndef MAX_INTERP_LEVEL
#if defined(XP_OS2)
#define MAX_INTERP_LEVEL 250
#else
#define MAX_INTERP_LEVEL 1000
#endif
#endif

#define MAX_INLINE_CALL_COUNT 1000

/*
 * Threaded interpretation via computed goto appears to be well-supported by
 * GCC 3 and higher.  IBM's C compiler when run with the right options (e.g.,
 * -qlanglvl=extended) also supports threading.  Ditto the SunPro C compiler.
 * Currently it's broken for JS_VERSION < 160, though this isn't worth fixing.
 * Add your compiler support macros here.
 */
#if JS_VERSION >= 160 && (                                                    \
    __GNUC__ >= 3 ||                                                          \
    (__IBMC__ >= 700 && defined __IBM_COMPUTED_GOTO) ||                       \
    __SUNPRO_C >= 0x570)
# define JS_THREADED_INTERP 1
#else
# undef JS_THREADED_INTERP
#endif

JSBool
js_Interpret(JSContext *cx, jsbytecode *pc, jsval *result)
{
    JSRuntime *rt;
    JSStackFrame *fp;
    JSScript *script;
    uintN inlineCallCount;
    JSObject *obj, *obj2, *parent;
    JSVersion currentVersion, originalVersion;
    JSBranchCallback onbranch;
    JSBool ok, cond;
    JSTrapHandler interruptHandler;
    jsint depth, len;
    jsval *sp, *newsp;
    void *mark;
    jsbytecode *endpc, *pc2;
    JSOp op, op2;
    jsatomid atomIndex;
    JSAtom *atom;
    uintN argc, attrs, flags, slot;
    jsval *vp, lval, rval, ltmp, rtmp;
    jsid id;
    JSObject *withobj, *iterobj;
    JSProperty *prop;
    JSScopeProperty *sprop;
    JSString *str, *str2;
    jsint i, j;
    jsdouble d, d2;
    JSClass *clasp;
    JSFunction *fun;
    JSType type;
#if !defined JS_THREADED_INTERP && defined DEBUG
    FILE *tracefp = NULL;
#endif
#if JS_HAS_EXPORT_IMPORT
    JSIdArray *ida;
#endif
    jsint low, high, off, npairs;
    JSBool match;
#if JS_HAS_GETTER_SETTER
    JSPropertyOp getter, setter;
#endif
    int stackDummy;

#ifdef __GNUC__
# define JS_EXTENSION __extension__
# define JS_EXTENSION_(s) __extension__ ({ s; })
#else
# define JS_EXTENSION
# define JS_EXTENSION_(s) s
#endif

#ifdef JS_THREADED_INTERP
    static void *normalJumpTable[] = {
# define OPDEF(op,val,name,token,length,nuses,ndefs,prec,format) \
        JS_EXTENSION &&L_##op,
# include "jsopcode.tbl"
# undef OPDEF
    };

    static void *interruptJumpTable[] = {
# define OPDEF(op,val,name,token,length,nuses,ndefs,prec,format)              \
        ((op != JSOP_PUSHOBJ)                                                 \
         ? JS_EXTENSION &&interrupt                                           \
         : JS_EXTENSION &&L_JSOP_PUSHOBJ),
# include "jsopcode.tbl"
# undef OPDEF
    };

    register void **jumpTable = normalJumpTable;

# define DO_OP()            JS_EXTENSION_(goto *jumpTable[op])
# define DO_NEXT_OP(n)      do { op = *(pc += (n)); DO_OP(); } while (0)
# define BEGIN_CASE(OP)     L_##OP:
# define END_CASE(OP)       DO_NEXT_OP(OP##_LENGTH);
# define END_VARLEN_CASE    DO_NEXT_OP(len);
# define EMPTY_CASE(OP)     BEGIN_CASE(OP) op = *++pc; DO_OP();
#else
# define DO_OP()            goto do_op
# define DO_NEXT_OP(n)      goto advance_pc
# define BEGIN_CASE(OP)     case OP:
# define END_CASE(OP)       break;
# define END_VARLEN_CASE    break;
# define EMPTY_CASE(OP)     BEGIN_CASE(OP) END_CASE(OP)
#endif

    *result = JSVAL_VOID;
    rt = cx->runtime;

    /* Set registerized frame pointer and derived script pointer. */
    fp = cx->fp;
    script = fp->script;
    JS_ASSERT(script->length != 0);

    /* Count of JS function calls that nest in this C js_Interpret frame. */
    inlineCallCount = 0;

    /*
     * Optimized Get and SetVersion for proper script language versioning.
     *
     * If any native method or JSClass/JSObjectOps hook calls js_SetVersion
     * and changes cx->version, the effect will "stick" and we will stop
     * maintaining currentVersion.  This is relied upon by testsuites, for
     * the most part -- web browsers select version before compiling and not
     * at run-time.
     */
    currentVersion = script->version;
    originalVersion = cx->version;
    if (currentVersion != originalVersion)
        js_SetVersion(cx, currentVersion);

#ifdef __GNUC__
    flags = 0;  /* suppress gcc warnings */
    id = 0;
#endif

    /*
     * Prepare to call a user-supplied branch handler, and abort the script
     * if it returns false.  We reload onbranch after calling out to native
     * functions (but not to getters, setters, or other native hooks).
     */
#define LOAD_BRANCH_CALLBACK(cx)    (onbranch = (cx)->branchCallback)

    LOAD_BRANCH_CALLBACK(cx);
#define CHECK_BRANCH(len)                                                     \
    JS_BEGIN_MACRO                                                            \
        if (len <= 0 && onbranch) {                                           \
            SAVE_SP_AND_PC(fp);                                               \
            if (!(ok = (*onbranch)(cx, script)))                              \
                goto out;                                                     \
        }                                                                     \
    JS_END_MACRO

    /*
     * Load the debugger's interrupt hook here and after calling out to native
     * functions (but not to getters, setters, or other native hooks), so we do
     * not have to reload it each time through the interpreter loop -- we hope
     * the compiler can keep it in a register when it is non-null.
     */
#ifdef JS_THREADED_INTERP
# define LOAD_JUMP_TABLE()                                                    \
    (jumpTable = interruptHandler ? interruptJumpTable : normalJumpTable)
#else
# define LOAD_JUMP_TABLE()      /* nothing */
#endif

#define LOAD_INTERRUPT_HANDLER(rt)                                            \
    JS_BEGIN_MACRO                                                            \
        interruptHandler = (rt)->interruptHandler;                            \
        LOAD_JUMP_TABLE();                                                    \
    JS_END_MACRO

    LOAD_INTERRUPT_HANDLER(rt);

    /* Check for too much js_Interpret nesting, or too deep a C stack. */
    if (++cx->interpLevel == MAX_INTERP_LEVEL ||
        !JS_CHECK_STACK_SIZE(cx, stackDummy)) {
        JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_OVER_RECURSED);
        ok = JS_FALSE;
        goto out2;
    }

    /*
     * Allocate operand and pc stack slots for the script's worst-case depth,
     * unless we're called to interpret a part of an already active script, a
     * filtering predicate expression for example.
     */
    depth = (jsint) script->depth;
    if (JS_LIKELY(!fp->spbase)) {
        newsp = js_AllocRawStack(cx, (uintN)(2 * depth), &mark);
        if (!newsp) {
            ok = JS_FALSE;
            goto out2;
        }
        sp = newsp + depth;
        fp->spbase = sp;
        SAVE_SP(fp);
    } else {
        sp = fp->sp;
        JS_ASSERT(JS_UPTRDIFF(sp, fp->spbase) <= depth * sizeof(jsval));
        newsp = fp->spbase - depth;
        mark = NULL;
    }

    /*
     * To support generator_throw and to catch ignored exceptions, fail right
     * away if cx->throwing is set.  If no exception is pending, null obj in
     * case a callable object is being sent into a yield expression, and the
     * yield's result is invoked.
     */
    ok = !cx->throwing;
    if (!ok) {
#ifdef DEBUG_NOT_THROWING
        printf("JS INTERPRETER CALLED WITH PENDING EXCEPTION %lx\n",
               (unsigned long) cx->exception);
#endif
        goto out;
    }
    obj = NULL;

#ifdef JS_THREADED_INTERP

    /*
     * This is a loop, but it does not look like a loop.  The loop-closing
     * jump is distributed throughout interruptJumpTable, and comes back to
     * the interrupt label.  The dispatch on op is through normalJumpTable.
     * The trick is LOAD_INTERRUPT_HANDLER setting jumpTable appropriately.
     *
     * It is important that "op" be initialized before the interrupt label
     * because it is possible for "op" to be specially assigned during the
     * normally processing of an opcode while looping (in particular, this
     * happens in JSOP_TRAP while debugging).  We rely on DO_NEXT_OP to
     * correctly manage "op" in all other cases.
     */
    op = (JSOp) *pc;
    if (interruptHandler) {
interrupt:
        SAVE_SP_AND_PC(fp);
        switch (interruptHandler(cx, script, pc, &rval,
                                 rt->interruptHandlerData)) {
          case JSTRAP_ERROR:
            ok = JS_FALSE;
            goto out;
          case JSTRAP_CONTINUE:
            break;
          case JSTRAP_RETURN:
            fp->rval = rval;
            goto out;
          case JSTRAP_THROW:
            cx->throwing = JS_TRUE;
            cx->exception = rval;
            ok = JS_FALSE;
            goto out;
          default:;
        }
        LOAD_INTERRUPT_HANDLER(rt);
    }

    JS_ASSERT((uintN)op < (uintN)JSOP_LIMIT);
    JS_EXTENSION_(goto *normalJumpTable[op]);

#else  /* !JS_THREADED_INTERP */

    for (;;) {
        op = (JSOp) *pc;
      do_op:
        len = js_CodeSpec[op].length;

#ifdef DEBUG
        tracefp = (FILE *) cx->tracefp;
        if (tracefp) {
            intN nuses, n;

            fprintf(tracefp, "%4u: ", js_PCToLineNumber(cx, script, pc));
            js_Disassemble1(cx, script, pc,
                            PTRDIFF(pc, script->code, jsbytecode), JS_FALSE,
                            tracefp);
            nuses = js_CodeSpec[op].nuses;
            if (nuses) {
                SAVE_SP_AND_PC(fp);
                for (n = -nuses; n < 0; n++) {
                    str = js_DecompileValueGenerator(cx, n, sp[n], NULL);
                    if (str) {
                        fprintf(tracefp, "%s %s",
                                (n == -nuses) ? "  inputs:" : ",",
                                JS_GetStringBytes(str));
                    }
                }
                fprintf(tracefp, " @ %d\n", sp - fp->spbase);
            }
        }
#endif /* DEBUG */

        if (interruptHandler && op != JSOP_PUSHOBJ) {
            SAVE_SP_AND_PC(fp);
            switch (interruptHandler(cx, script, pc, &rval,
                                     rt->interruptHandlerData)) {
              case JSTRAP_ERROR:
                ok = JS_FALSE;
                goto out;
              case JSTRAP_CONTINUE:
                break;
              case JSTRAP_RETURN:
                fp->rval = rval;
                goto out;
              case JSTRAP_THROW:
                cx->throwing = JS_TRUE;
                cx->exception = rval;
                ok = JS_FALSE;
                goto out;
              default:;
            }
            LOAD_INTERRUPT_HANDLER(rt);
        }

        switch (op) {

#endif /* !JS_THREADED_INTERP */

          BEGIN_CASE(JSOP_STOP)
            goto out;

          EMPTY_CASE(JSOP_NOP)

          BEGIN_CASE(JSOP_GROUP)
            obj = NULL;
          END_CASE(JSOP_GROUP)

          BEGIN_CASE(JSOP_PUSH)
            PUSH_OPND(JSVAL_VOID);
          END_CASE(JSOP_PUSH)

          BEGIN_CASE(JSOP_POP)
            sp--;
          END_CASE(JSOP_POP)

          BEGIN_CASE(JSOP_POP2)
            sp -= 2;
          END_CASE(JSOP_POP2)

          BEGIN_CASE(JSOP_SWAP)
            vp = sp - depth;    /* swap generating pc's for the decompiler */
            ltmp = vp[-1];
            vp[-1] = vp[-2];
            sp[-2] = ltmp;
            rtmp = sp[-1];
            sp[-1] = sp[-2];
            sp[-2] = rtmp;
          END_CASE(JSOP_SWAP)

          BEGIN_CASE(JSOP_POPV)
            *result = POP_OPND();
          END_CASE(JSOP_POPV)

          BEGIN_CASE(JSOP_ENTERWITH)
            FETCH_OBJECT(cx, -1, rval, obj);
            SAVE_SP_AND_PC(fp);
            OBJ_TO_INNER_OBJECT(cx, obj);
            if (!obj || !(obj2 = js_GetScopeChain(cx, fp))) {
                ok = JS_FALSE;
                goto out;
            }
            withobj = js_NewWithObject(cx, obj, obj2, sp - fp->spbase - 1);
            if (!withobj) {
                ok = JS_FALSE;
                goto out;
            }
            fp->scopeChain = withobj;
            STORE_OPND(-1, OBJECT_TO_JSVAL(withobj));
          END_CASE(JSOP_ENTERWITH)

          BEGIN_CASE(JSOP_LEAVEWITH)
            rval = POP_OPND();
            JS_ASSERT(JSVAL_IS_OBJECT(rval));
            withobj = JSVAL_TO_OBJECT(rval);
            JS_ASSERT(OBJ_GET_CLASS(cx, withobj) == &js_WithClass);
            fp->scopeChain = OBJ_GET_PARENT(cx, withobj);
            JS_SetPrivate(cx, withobj, NULL);
          END_CASE(JSOP_LEAVEWITH)

          BEGIN_CASE(JSOP_SETRVAL)
            ASSERT_NOT_THROWING(cx);
            fp->rval = POP_OPND();
          END_CASE(JSOP_SETRVAL)

          BEGIN_CASE(JSOP_RETURN)
            CHECK_BRANCH(-1);
            fp->rval = POP_OPND();
            /* FALL THROUGH */

          BEGIN_CASE(JSOP_RETRVAL)    /* fp->rval already set */
            ASSERT_NOT_THROWING(cx);
            if (inlineCallCount)
          inline_return:
            {
                JSInlineFrame *ifp = (JSInlineFrame *) fp;
                void *hookData = ifp->hookData;

                /*
                 * If fp has blocks on its scope chain, home their locals now,
                 * before calling any debugger hook, and before freeing stack.
                 * This matches the order of block putting and hook calling in
                 * the "out-of-line" return code at the bottom of js_Interpret
                 * and in js_Invoke.
                 */
                if (fp->flags & JSFRAME_POP_BLOCKS) {
                    SAVE_SP_AND_PC(fp);
                    ok &= PutBlockObjects(cx, fp);
                }

                if (hookData) {
                    JSInterpreterHook hook = rt->callHook;
                    if (hook) {
                        SAVE_SP_AND_PC(fp);
                        hook(cx, fp, JS_FALSE, &ok, hookData);
                        LOAD_INTERRUPT_HANDLER(rt);
                    }
                }

                /*
                 * If fp has a call object, sync values and clear the back-
                 * pointer. This can happen for a lightweight function if it
                 * calls eval unexpectedly (in a way that is hidden from the
                 * compiler). See bug 325540.
                 */
                if (fp->callobj) {
                    SAVE_SP_AND_PC(fp);
                    ok &= js_PutCallObject(cx, fp);
                }

                if (fp->argsobj) {
                    SAVE_SP_AND_PC(fp);
                    ok &= js_PutArgsObject(cx, fp);
                }

                /* Restore context version only if callee hasn't set version. */
                if (JS_LIKELY(cx->version == currentVersion)) {
                    currentVersion = ifp->callerVersion;
                    if (currentVersion != cx->version)
                        js_SetVersion(cx, currentVersion);
                }

                /* Store the return value in the caller's operand frame. */
                vp = ifp->rvp;
                *vp = fp->rval;

                /* Restore cx->fp and release the inline frame's space. */
                cx->fp = fp = fp->down;
                JS_ARENA_RELEASE(&cx->stackPool, ifp->mark);

                /* Restore sp to point just above the return value. */
                fp->sp = vp + 1;
                RESTORE_SP(fp);

                /* Restore the calling script's interpreter registers. */
                obj = NULL;
                script = fp->script;
                depth = (jsint) script->depth;
                pc = fp->pc;
#ifndef JS_THREADED_INTERP
                endpc = script->code + script->length;
#endif

                /* Store the generating pc for the return value. */
                vp[-depth] = (jsval)pc;

                /* Resume execution in the calling frame. */
                inlineCallCount--;
                if (JS_LIKELY(ok)) {
                    JS_ASSERT(js_CodeSpec[*pc].length == JSOP_CALL_LENGTH);
                    len = JSOP_CALL_LENGTH;
                    DO_NEXT_OP(len);
                }
            }
            goto out;

          BEGIN_CASE(JSOP_DEFAULT)
            (void) POP();
            /* FALL THROUGH */
          BEGIN_CASE(JSOP_GOTO)
            len = GET_JUMP_OFFSET(pc);
            CHECK_BRANCH(len);
          END_VARLEN_CASE

          BEGIN_CASE(JSOP_IFEQ)
            POP_BOOLEAN(cx, rval, cond);
            if (cond == JS_FALSE) {
                len = GET_JUMP_OFFSET(pc);
                CHECK_BRANCH(len);
                DO_NEXT_OP(len);
            }
          END_CASE(JSOP_IFEQ)

          BEGIN_CASE(JSOP_IFNE)
            POP_BOOLEAN(cx, rval, cond);
            if (cond != JS_FALSE) {
                len = GET_JUMP_OFFSET(pc);
                CHECK_BRANCH(len);
                DO_NEXT_OP(len);
            }
          END_CASE(JSOP_IFNE)

          BEGIN_CASE(JSOP_OR)
            POP_BOOLEAN(cx, rval, cond);
            if (cond == JS_TRUE) {
                len = GET_JUMP_OFFSET(pc);
                PUSH_OPND(rval);
                DO_NEXT_OP(len);
            }
          END_CASE(JSOP_OR)

          BEGIN_CASE(JSOP_AND)
            POP_BOOLEAN(cx, rval, cond);
            if (cond == JS_FALSE) {
                len = GET_JUMP_OFFSET(pc);
                PUSH_OPND(rval);
                DO_NEXT_OP(len);
            }
          END_CASE(JSOP_AND)

          BEGIN_CASE(JSOP_DEFAULTX)
            (void) POP();
            /* FALL THROUGH */
          BEGIN_CASE(JSOP_GOTOX)
            len = GET_JUMPX_OFFSET(pc);
            CHECK_BRANCH(len);
          END_VARLEN_CASE

          BEGIN_CASE(JSOP_IFEQX)
            POP_BOOLEAN(cx, rval, cond);
            if (cond == JS_FALSE) {
                len = GET_JUMPX_OFFSET(pc);
                CHECK_BRANCH(len);
                DO_NEXT_OP(len);
            }
          END_CASE(JSOP_IFEQX)

          BEGIN_CASE(JSOP_IFNEX)
            POP_BOOLEAN(cx, rval, cond);
            if (cond != JS_FALSE) {
                len = GET_JUMPX_OFFSET(pc);
                CHECK_BRANCH(len);
                DO_NEXT_OP(len);
            }
          END_CASE(JSOP_IFNEX)

          BEGIN_CASE(JSOP_ORX)
            POP_BOOLEAN(cx, rval, cond);
            if (cond == JS_TRUE) {
                len = GET_JUMPX_OFFSET(pc);
                PUSH_OPND(rval);
                DO_NEXT_OP(len);
            }
          END_CASE(JSOP_ORX)

          BEGIN_CASE(JSOP_ANDX)
            POP_BOOLEAN(cx, rval, cond);
            if (cond == JS_FALSE) {
                len = GET_JUMPX_OFFSET(pc);
                PUSH_OPND(rval);
                DO_NEXT_OP(len);
            }
          END_CASE(JSOP_ANDX)

/*
 * If the index value at sp[n] is not an int that fits in a jsval, it could
 * be an object (an XML QName, AttributeName, or AnyName), but only if we are
 * compiling with JS_HAS_XML_SUPPORT.  Otherwise convert the index value to a
 * string atom id.
 */
#define FETCH_ELEMENT_ID(n, id)                                               \
    JS_BEGIN_MACRO                                                            \
        jsval idval_ = FETCH_OPND(n);                                         \
        if (JSVAL_IS_INT(idval_)) {                                           \
            id = INT_JSVAL_TO_JSID(idval_);                                   \
        } else {                                                              \
            SAVE_SP_AND_PC(fp);                                               \
            ok = InternNonIntElementId(cx, idval_, &id);                      \
            if (!ok)                                                          \
                goto out;                                                     \
        }                                                                     \
    JS_END_MACRO

          BEGIN_CASE(JSOP_IN)
            SAVE_SP_AND_PC(fp);
            rval = FETCH_OPND(-1);
            if (JSVAL_IS_PRIMITIVE(rval)) {
                str = js_DecompileValueGenerator(cx, -1, rval, NULL);
                if (str) {
                    JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL,
                                         JSMSG_IN_NOT_OBJECT,
                                         JS_GetStringBytes(str));
                }
                ok = JS_FALSE;
                goto out;
            }
            obj = JSVAL_TO_OBJECT(rval);
            FETCH_ELEMENT_ID(-2, id);
            CHECK_ELEMENT_ID(obj, id);
            ok = OBJ_LOOKUP_PROPERTY(cx, obj, id, &obj2, &prop);
            if (!ok)
                goto out;
            sp--;
            STORE_OPND(-1, BOOLEAN_TO_JSVAL(prop != NULL));
            if (prop)
                OBJ_DROP_PROPERTY(cx, obj2, prop);
          END_CASE(JSOP_IN)

          BEGIN_CASE(JSOP_FOREACH)
            flags = JSITER_ENUMERATE | JSITER_FOREACH;
            goto value_to_iter;

#if JS_HAS_DESTRUCTURING
          BEGIN_CASE(JSOP_FOREACHKEYVAL)
            flags = JSITER_ENUMERATE | JSITER_FOREACH | JSITER_KEYVALUE;
            goto value_to_iter;
#endif

          BEGIN_CASE(JSOP_FORIN)
            /*
             * Set JSITER_ENUMERATE to indicate that for-in loop should use
             * the enumeration protocol's iterator for compatibility if an
             * explicit iterator is not given via the optional __iterator__
             * method.
             */
            flags = JSITER_ENUMERATE;

          value_to_iter:
            JS_ASSERT(sp > fp->spbase);
            SAVE_SP_AND_PC(fp);
            ok = js_ValueToIterator(cx, flags, &sp[-1]);
            if (!ok)
                goto out;
            JS_ASSERT(!JSVAL_IS_PRIMITIVE(sp[-1]));
            JS_ASSERT(JSOP_FORIN_LENGTH == js_CodeSpec[op].length);
          END_CASE(JSOP_FORIN)

          BEGIN_CASE(JSOP_FORPROP)
            /*
             * Handle JSOP_FORPROP first, so the cost of the goto do_forinloop
             * is not paid for the more common cases.
             */
            lval = FETCH_OPND(-1);
            atom = GET_ATOM(cx, script, pc);
            id   = ATOM_TO_JSID(atom);
            i = -2;
            goto do_forinloop;

          BEGIN_CASE(JSOP_FORNAME)
            atom = GET_ATOM(cx, script, pc);
            id   = ATOM_TO_JSID(atom);

            /*
             * ECMA 12.6.3 says to eval the LHS after looking for properties
             * to enumerate, and bail without LHS eval if there are no props.
             * We do Find here to share the most code at label do_forinloop.
             * If looking for enumerable properties could have side effects,
             * then we'd have to move this into the common code and condition
             * it on op == JSOP_FORNAME.
             */
            SAVE_SP_AND_PC(fp);
            ok = js_FindProperty(cx, id, &obj, &obj2, &prop);
            if (!ok)
                goto out;
            if (prop)
                OBJ_DROP_PROPERTY(cx, obj2, prop);
            lval = OBJECT_TO_JSVAL(obj);
            /* FALL THROUGH */

          BEGIN_CASE(JSOP_FORARG)
          BEGIN_CASE(JSOP_FORVAR)
          BEGIN_CASE(JSOP_FORLOCAL)
            /*
             * JSOP_FORARG and JSOP_FORVAR don't require any lval computation
             * here, because they address slots on the stack (in fp->args and
             * fp->vars, respectively).  Same applies to JSOP_FORLOCAL, which
             * addresses fp->spbase.
             */
            /* FALL THROUGH */

          BEGIN_CASE(JSOP_FORELEM)
            /*
             * JSOP_FORELEM simply initializes or updates the iteration state
             * and leaves the index expression evaluation and assignment to the
             * enumerator until after the next property has been acquired, via
             * a JSOP_ENUMELEM bytecode.
             */
            i = -1;

          do_forinloop:
            /*
             * Reach under the top of stack to find our property iterator, a
             * JSObject that contains the iteration state.
             */
            JS_ASSERT(!JSVAL_IS_PRIMITIVE(sp[i]));
            iterobj = JSVAL_TO_OBJECT(sp[i]);
          
            SAVE_SP_AND_PC(fp);
            ok = js_CallIteratorNext(cx, iterobj, &rval);
            if (!ok)
                goto out;
            if (rval == JSVAL_HOLE) {
                rval = JSVAL_FALSE;
                goto end_forinloop;
            }

            switch (op) {
              case JSOP_FORARG:
                slot = GET_ARGNO(pc);
                JS_ASSERT(slot < fp->fun->nargs);
                fp->argv[slot] = rval;
                break;

              case JSOP_FORVAR:
                slot = GET_VARNO(pc);
                JS_ASSERT(slot < fp->fun->u.i.nvars);
                fp->vars[slot] = rval;
                break;

              case JSOP_FORLOCAL:
                slot = GET_UINT16(pc);
                JS_ASSERT(slot < (uintN)depth);
                vp = &fp->spbase[slot];
                GC_POKE(cx, *vp);
                *vp = rval;
                break;

              case JSOP_FORELEM:
                /* FORELEM is not a SET operation, it's more like BINDNAME. */
                PUSH_OPND(rval);
                break;

              default:
                JS_ASSERT(op == JSOP_FORPROP || op == JSOP_FORNAME);

                /* Convert lval to a non-null object containing id. */
                VALUE_TO_OBJECT(cx, lval, obj);
                if (op == JSOP_FORPROP)
                    STORE_OPND(-1, OBJECT_TO_JSVAL(obj));

                /* Set the variable obj[id] to refer to rval. */
                fp->flags |= JSFRAME_ASSIGNING;
                ok = OBJ_SET_PROPERTY(cx, obj, id, &rval);
                fp->flags &= ~JSFRAME_ASSIGNING;
                if (!ok)
                    goto out;
                break;
            }

            /* Push true to keep looping through properties. */
            rval = JSVAL_TRUE;

          end_forinloop:
            sp += i + 1;
            PUSH_OPND(rval);
            len = js_CodeSpec[op].length;
            DO_NEXT_OP(len);

          BEGIN_CASE(JSOP_DUP)
            JS_ASSERT(sp > fp->spbase);
            vp = sp - 1;                /* address top of stack */
            rval = *vp;
            vp -= depth;                /* address generating pc */
            vp[1] = *vp;
            PUSH(rval);
          END_CASE(JSOP_DUP)

          BEGIN_CASE(JSOP_DUP2)
            JS_ASSERT(sp - 2 >= fp->spbase);
            vp = sp - 1;                /* address top of stack */
            lval = vp[-1];
            rval = *vp;
            vp -= depth;                /* address generating pc */
            vp[1] = vp[2] = *vp;
            PUSH(lval);
            PUSH(rval);
          END_CASE(JSOP_DUP2)

#define PROPERTY_OP(n, call)                                                  \
    JS_BEGIN_MACRO                                                            \
        /* Fetch the left part and resolve it to a non-null object. */        \
        FETCH_OBJECT(cx, n, lval, obj);                                       \
                                                                              \
        /* Get or set the property, set ok false if error, true if success. */\
        SAVE_SP_AND_PC(fp);                                                   \
        call;                                                                 \
        if (!ok)                                                              \
            goto out;                                                         \
    JS_END_MACRO

#define ELEMENT_OP(n, call)                                                   \
    JS_BEGIN_MACRO                                                            \
        /* Fetch the right part and resolve it to an internal id. */          \
        FETCH_ELEMENT_ID(n, id);                                              \
                                                                              \
        /* Fetch the left part and resolve it to a non-null object. */        \
        FETCH_OBJECT(cx, n - 1, lval, obj);                                   \
                                                                              \
        /* Ensure that id has a type suitable for use with obj. */            \
        CHECK_ELEMENT_ID(obj, id);                                            \
                                                                              \
        /* Get or set the element, set ok false if error, true if success. */ \
        SAVE_SP_AND_PC(fp);                                                   \
        call;                                                                 \
        if (!ok)                                                              \
            goto out;                                                         \
    JS_END_MACRO

#define NATIVE_GET(cx,obj,pobj,sprop,vp)                                      \
    JS_BEGIN_MACRO                                                            \
        if (SPROP_HAS_STUB_GETTER(sprop)) {                                   \
            /* Fast path for Object instance properties. */                   \
            JS_ASSERT((sprop)->slot != SPROP_INVALID_SLOT ||                  \
                      !SPROP_HAS_STUB_SETTER(sprop));                         \
            *vp = ((sprop)->slot != SPROP_INVALID_SLOT)                       \
                  ? LOCKED_OBJ_GET_SLOT(pobj, (sprop)->slot)                  \
                  : JSVAL_VOID;                                               \
        } else {                                                              \
            SAVE_SP_AND_PC(fp);                                               \
            ok = js_NativeGet(cx, obj, pobj, sprop, vp);                      \
            if (!ok)                                                          \
                goto out;                                                     \
        }                                                                     \
    JS_END_MACRO

#define NATIVE_SET(cx,obj,sprop,vp)                                           \
    JS_BEGIN_MACRO                                                            \
        if (SPROP_HAS_STUB_SETTER(sprop) &&                                   \
            (sprop)->slot != SPROP_INVALID_SLOT) {                            \
            /* Fast path for Object instance properties. */                   \
            LOCKED_OBJ_SET_SLOT(obj, (sprop)->slot, *vp);                     \
        } else {                                                              \
            SAVE_SP_AND_PC(fp);                                               \
            ok = js_NativeSet(cx, obj, sprop, vp);                            \
            if (!ok)                                                          \
                goto out;                                                     \
        }                                                                     \
    JS_END_MACRO

/*
 * CACHED_GET and CACHED_SET use cx, obj, id, and rval from their callers'
 * environments.
 */
#define CACHED_GET(call)                                                      \
    JS_BEGIN_MACRO                                                            \
        if (!OBJ_IS_NATIVE(obj)) {                                            \
            ok = call;                                                        \
        } else {                                                              \
            JS_LOCK_OBJ(cx, obj);                                             \
            PROPERTY_CACHE_TEST(&rt->propertyCache, obj, id, sprop);          \
            if (sprop) {                                                      \
                NATIVE_GET(cx, obj, obj, sprop, &rval);                       \
                JS_UNLOCK_OBJ(cx, obj);                                       \
            } else {                                                          \
                JS_UNLOCK_OBJ(cx, obj);                                       \
                ok = call;                                                    \
                /* No fill here: js_GetProperty fills the cache. */           \
            }                                                                 \
        }                                                                     \
    JS_END_MACRO

#define CACHED_SET(call)                                                      \
    JS_BEGIN_MACRO                                                            \
        if (!OBJ_IS_NATIVE(obj)) {                                            \
            ok = call;                                                        \
        } else {                                                              \
            JSScope *scope_;                                                  \
            JS_LOCK_OBJ(cx, obj);                                             \
            PROPERTY_CACHE_TEST(&rt->propertyCache, obj, id, sprop);          \
            if (sprop &&                                                      \
                !(sprop->attrs & JSPROP_READONLY) &&                          \
                (scope_ = OBJ_SCOPE(obj), !SCOPE_IS_SEALED(scope_))) {        \
                NATIVE_SET(cx, obj, sprop, &rval);                            \
                JS_UNLOCK_SCOPE(cx, scope_);                                  \
            } else {                                                          \
                JS_UNLOCK_OBJ(cx, obj);                                       \
                ok = call;                                                    \
                /* No fill here: js_SetProperty writes through the cache. */  \
            }                                                                 \
        }                                                                     \
    JS_END_MACRO

#define BEGIN_LITOPX_CASE(OP,PCOFF)                                           \
          BEGIN_CASE(OP)                                                      \
            pc2 = pc;                                                         \
            atomIndex = GET_ATOM_INDEX(pc + PCOFF);                           \
          do_##OP:                                                            \
            atom = js_GetAtom(cx, &script->atomMap, atomIndex);

#define END_LITOPX_CASE(OP)                                                   \
          END_CASE(OP)

          BEGIN_LITOPX_CASE(JSOP_SETCONST, 0)
            obj = fp->varobj;
            rval = FETCH_OPND(-1);
            SAVE_SP_AND_PC(fp);
            ok = OBJ_DEFINE_PROPERTY(cx, obj, ATOM_TO_JSID(atom), rval,
                                     NULL, NULL,
                                     JSPROP_ENUMERATE | JSPROP_PERMANENT |
                                     JSPROP_READONLY,
                                     NULL);
            if (!ok)
                goto out;
            STORE_OPND(-1, rval);
          END_LITOPX_CASE(JSOP_SETCONST)

#if JS_HAS_DESTRUCTURING
          BEGIN_CASE(JSOP_ENUMCONSTELEM)
            FETCH_ELEMENT_ID(-1, id);
            FETCH_OBJECT(cx, -2, lval, obj);
            CHECK_ELEMENT_ID(obj, id);
            rval = FETCH_OPND(-3);
            SAVE_SP_AND_PC(fp);
            ok = OBJ_DEFINE_PROPERTY(cx, obj, id, rval, NULL, NULL,
                                     JSPROP_ENUMERATE | JSPROP_PERMANENT |
                                     JSPROP_READONLY,
                                     NULL);
            if (!ok)
                goto out;
            sp -= 3;
          END_CASE(JSOP_ENUMCONSTELEM)
#endif

          BEGIN_LITOPX_CASE(JSOP_BINDNAME, 0)
            SAVE_SP_AND_PC(fp);
            obj = js_FindIdentifierBase(cx, ATOM_TO_JSID(atom));
            if (!obj) {
                ok = JS_FALSE;
                goto out;
            }
            PUSH_OPND(OBJECT_TO_JSVAL(obj));
          END_LITOPX_CASE(JSOP_BINDNAME)

          BEGIN_CASE(JSOP_SETNAME)
            atom = GET_ATOM(cx, script, pc);
            id   = ATOM_TO_JSID(atom);
            rval = FETCH_OPND(-1);
            lval = FETCH_OPND(-2);
            JS_ASSERT(!JSVAL_IS_PRIMITIVE(lval));
            obj  = JSVAL_TO_OBJECT(lval);
            SAVE_SP_AND_PC(fp);
            CACHED_SET(OBJ_SET_PROPERTY(cx, obj, id, &rval));
            if (!ok)
                goto out;
            sp--;
            STORE_OPND(-1, rval);
            obj = NULL;
          END_CASE(JSOP_SETNAME)

#define INTEGER_OP(OP, EXTRA_CODE)                                            \
    JS_BEGIN_MACRO                                                            \
        FETCH_INT(cx, -1, j);                                                 \
        FETCH_INT(cx, -2, i);                                                 \
        EXTRA_CODE                                                            \
        i = i OP j;                                                           \
        sp--;                                                                 \
        STORE_INT(cx, -1, i);                                                 \
    JS_END_MACRO

#define BITWISE_OP(OP)          INTEGER_OP(OP, (void) 0;)
#define SIGNED_SHIFT_OP(OP)     INTEGER_OP(OP, j &= 31;)

          BEGIN_CASE(JSOP_BITOR)
            BITWISE_OP(|);
          END_CASE(JSOP_BITOR)

          BEGIN_CASE(JSOP_BITXOR)
            BITWISE_OP(^);
          END_CASE(JSOP_BITXOR)

          BEGIN_CASE(JSOP_BITAND)
            BITWISE_OP(&);
          END_CASE(JSOP_BITAND)

#define RELATIONAL_OP(OP)                                                     \
    JS_BEGIN_MACRO                                                            \
        rval = FETCH_OPND(-1);                                                \
        lval = FETCH_OPND(-2);                                                \
        /* Optimize for two int-tagged operands (typical loop control). */    \
        if ((lval & rval) & JSVAL_INT) {                                      \
            ltmp = lval ^ JSVAL_VOID;                                         \
            rtmp = rval ^ JSVAL_VOID;                                         \
            if (ltmp && rtmp) {                                               \
                cond = JSVAL_TO_INT(lval) OP JSVAL_TO_INT(rval);              \
            } else {                                                          \
                d  = ltmp ? JSVAL_TO_INT(lval) : *rt->jsNaN;                  \
                d2 = rtmp ? JSVAL_TO_INT(rval) : *rt->jsNaN;                  \
                cond = JSDOUBLE_COMPARE(d, OP, d2, JS_FALSE);                 \
            }                                                                 \
        } else {                                                              \
            VALUE_TO_PRIMITIVE(cx, lval, JSTYPE_NUMBER, &lval);               \
            sp[-2] = lval;                                                    \
            VALUE_TO_PRIMITIVE(cx, rval, JSTYPE_NUMBER, &rval);               \
            if (JSVAL_IS_STRING(lval) && JSVAL_IS_STRING(rval)) {             \
                str  = JSVAL_TO_STRING(lval);                                 \
                str2 = JSVAL_TO_STRING(rval);                                 \
                cond = js_CompareStrings(str, str2) OP 0;                     \
            } else {                                                          \
                VALUE_TO_NUMBER(cx, lval, d);                                 \
                VALUE_TO_NUMBER(cx, rval, d2);                                \
                cond = JSDOUBLE_COMPARE(d, OP, d2, JS_FALSE);                 \
            }                                                                 \
        }                                                                     \
        sp--;                                                                 \
        STORE_OPND(-1, BOOLEAN_TO_JSVAL(cond));                               \
    JS_END_MACRO

/*
 * NB: These macros can't use JS_BEGIN_MACRO/JS_END_MACRO around their bodies
 * because they begin if/else chains, so callers must not put semicolons after
 * the call expressions!
 */
#if JS_HAS_XML_SUPPORT
#define XML_EQUALITY_OP(OP)                                                   \
    if ((ltmp == JSVAL_OBJECT &&                                              \
         (obj2 = JSVAL_TO_OBJECT(lval)) &&                                    \
         OBJECT_IS_XML(cx, obj2)) ||                                          \
        (rtmp == JSVAL_OBJECT &&                                              \
         (obj2 = JSVAL_TO_OBJECT(rval)) &&                                    \
         OBJECT_IS_XML(cx, obj2))) {                                          \
        JSXMLObjectOps *ops;                                                  \
                                                                              \
        ops = (JSXMLObjectOps *) obj2->map->ops;                              \
        if (obj2 == JSVAL_TO_OBJECT(rval))                                    \
            rval = lval;                                                      \
        SAVE_SP_AND_PC(fp);                                                   \
        ok = ops->equality(cx, obj2, rval, &cond);                            \
        if (!ok)                                                              \
            goto out;                                                         \
        cond = cond OP JS_TRUE;                                               \
    } else

#define EXTENDED_EQUALITY_OP(OP)                                              \
    if (ltmp == JSVAL_OBJECT &&                                               \
        (obj2 = JSVAL_TO_OBJECT(lval)) &&                                     \
        ((clasp = OBJ_GET_CLASS(cx, obj2))->flags & JSCLASS_IS_EXTENDED)) {   \
        JSExtendedClass *xclasp;                                              \
                                                                              \
        xclasp = (JSExtendedClass *) clasp;                                   \
        SAVE_SP_AND_PC(fp);                                                   \
        ok = xclasp->equality(cx, obj2, rval, &cond);                         \
        if (!ok)                                                              \
            goto out;                                                         \
        cond = cond OP JS_TRUE;                                               \
    } else
#else
#define XML_EQUALITY_OP(OP)             /* nothing */
#define EXTENDED_EQUALITY_OP(OP)        /* nothing */
#endif

#define EQUALITY_OP(OP, IFNAN)                                                \
    JS_BEGIN_MACRO                                                            \
        rval = FETCH_OPND(-1);                                                \
        lval = FETCH_OPND(-2);                                                \
        ltmp = JSVAL_TAG(lval);                                               \
        rtmp = JSVAL_TAG(rval);                                               \
        XML_EQUALITY_OP(OP)                                                   \
        if (ltmp == rtmp) {                                                   \
            if (ltmp == JSVAL_STRING) {                                       \
                str  = JSVAL_TO_STRING(lval);                                 \
                str2 = JSVAL_TO_STRING(rval);                                 \
                cond = js_EqualStrings(str, str2) OP JS_TRUE;                 \
            } else if (ltmp == JSVAL_DOUBLE) {                                \
                d  = *JSVAL_TO_DOUBLE(lval);                                  \
                d2 = *JSVAL_TO_DOUBLE(rval);                                  \
                cond = JSDOUBLE_COMPARE(d, OP, d2, IFNAN);                    \
            } else {                                                          \
                EXTENDED_EQUALITY_OP(OP)                                      \
                /* Handle all undefined (=>NaN) and int combinations. */      \
                cond = lval OP rval;                                          \
            }                                                                 \
        } else {                                                              \
            if (JSVAL_IS_NULL(lval) || JSVAL_IS_VOID(lval)) {                 \
                cond = (JSVAL_IS_NULL(rval) || JSVAL_IS_VOID(rval)) OP 1;     \
            } else if (JSVAL_IS_NULL(rval) || JSVAL_IS_VOID(rval)) {          \
                cond = 1 OP 0;                                                \
            } else {                                                          \
                if (ltmp == JSVAL_OBJECT) {                                   \
                    VALUE_TO_PRIMITIVE(cx, lval, JSTYPE_VOID, &sp[-2]);       \
                    lval = sp[-2];                                            \
                    ltmp = JSVAL_TAG(lval);                                   \
                } else if (rtmp == JSVAL_OBJECT) {                            \
                    VALUE_TO_PRIMITIVE(cx, rval, JSTYPE_VOID, &sp[-1]);       \
                    rval = sp[-1];                                            \
                    rtmp = JSVAL_TAG(rval);                                   \
                }                                                             \
                if (ltmp == JSVAL_STRING && rtmp == JSVAL_STRING) {           \
                    str  = JSVAL_TO_STRING(lval);                             \
                    str2 = JSVAL_TO_STRING(rval);                             \
                    cond = js_EqualStrings(str, str2) OP JS_TRUE;             \
                } else {                                                      \
                    VALUE_TO_NUMBER(cx, lval, d);                             \
                    VALUE_TO_NUMBER(cx, rval, d2);                            \
                    cond = JSDOUBLE_COMPARE(d, OP, d2, IFNAN);                \
                }                                                             \
            }                                                                 \
        }                                                                     \
        sp--;                                                                 \
        STORE_OPND(-1, BOOLEAN_TO_JSVAL(cond));                               \
    JS_END_MACRO

          BEGIN_CASE(JSOP_EQ)
            EQUALITY_OP(==, JS_FALSE);
          END_CASE(JSOP_EQ)

          BEGIN_CASE(JSOP_NE)
            EQUALITY_OP(!=, JS_TRUE);
          END_CASE(JSOP_NE)

#define NEW_EQUALITY_OP(OP)                                                   \
    JS_BEGIN_MACRO                                                            \
        rval = FETCH_OPND(-1);                                                \
        lval = FETCH_OPND(-2);                                                \
        cond = js_StrictlyEqual(lval, rval) OP JS_TRUE;                       \
        sp--;                                                                 \
        STORE_OPND(-1, BOOLEAN_TO_JSVAL(cond));                               \
    JS_END_MACRO

          BEGIN_CASE(JSOP_NEW_EQ)
            NEW_EQUALITY_OP(==);
          END_CASE(JSOP_NEW_EQ)

          BEGIN_CASE(JSOP_NEW_NE)
            NEW_EQUALITY_OP(!=);
          END_CASE(JSOP_NEW_NE)

          BEGIN_CASE(JSOP_CASE)
            pc2 = (jsbytecode *) sp[-2-depth];
            NEW_EQUALITY_OP(==);
            (void) POP();
            if (cond) {
                len = GET_JUMP_OFFSET(pc);
                CHECK_BRANCH(len);
                DO_NEXT_OP(len);
            }
            sp[-depth] = (jsval)pc2;
            PUSH(lval);
          END_CASE(JSOP_CASE)

          BEGIN_CASE(JSOP_CASEX)
            pc2 = (jsbytecode *) sp[-2-depth];
            NEW_EQUALITY_OP(==);
            (void) POP();
            if (cond) {
                len = GET_JUMPX_OFFSET(pc);
                CHECK_BRANCH(len);
                DO_NEXT_OP(len);
            }
            sp[-depth] = (jsval)pc2;
            PUSH(lval);
          END_CASE(JSOP_CASEX)

          BEGIN_CASE(JSOP_LT)
            RELATIONAL_OP(<);
          END_CASE(JSOP_LT)

          BEGIN_CASE(JSOP_LE)
            RELATIONAL_OP(<=);
          END_CASE(JSOP_LE)

          BEGIN_CASE(JSOP_GT)
            RELATIONAL_OP(>);
          END_CASE(JSOP_GT)

          BEGIN_CASE(JSOP_GE)
            RELATIONAL_OP(>=);
          END_CASE(JSOP_GE)

#undef EQUALITY_OP
#undef RELATIONAL_OP

          BEGIN_CASE(JSOP_LSH)
            SIGNED_SHIFT_OP(<<);
          END_CASE(JSOP_LSH)

          BEGIN_CASE(JSOP_RSH)
            SIGNED_SHIFT_OP(>>);
          END_CASE(JSOP_RSH)

          BEGIN_CASE(JSOP_URSH)
          {
            uint32 u;

            FETCH_INT(cx, -1, j);
            FETCH_UINT(cx, -2, u);
            u >>= j & 31;
            sp--;
            STORE_UINT(cx, -1, u);
          }
          END_CASE(JSOP_URSH)

#undef INTEGER_OP
#undef BITWISE_OP
#undef SIGNED_SHIFT_OP

          BEGIN_CASE(JSOP_ADD)
            rval = FETCH_OPND(-1);
            lval = FETCH_OPND(-2);
#if JS_HAS_XML_SUPPORT
            if (!JSVAL_IS_PRIMITIVE(lval) &&
                (obj2 = JSVAL_TO_OBJECT(lval), OBJECT_IS_XML(cx, obj2)) &&
                VALUE_IS_XML(cx, rval)) {
                JSXMLObjectOps *ops;

                ops = (JSXMLObjectOps *) obj2->map->ops;
                SAVE_SP_AND_PC(fp);
                ok = ops->concatenate(cx, obj2, rval, &rval);
                if (!ok)
                    goto out;
                sp--;
                STORE_OPND(-1, rval);
            } else
#endif
            {
                VALUE_TO_PRIMITIVE(cx, lval, JSTYPE_VOID, &sp[-2]);
                lval = sp[-2];
                VALUE_TO_PRIMITIVE(cx, rval, JSTYPE_VOID, &sp[-1]);
                rval = sp[-1];
                if ((cond = JSVAL_IS_STRING(lval)) || JSVAL_IS_STRING(rval)) {
                    SAVE_SP_AND_PC(fp);
                    if (cond) {
                        str = JSVAL_TO_STRING(lval);
                        ok = (str2 = js_ValueToString(cx, rval)) != NULL;
                        if (!ok)
                            goto out;
                        sp[-1] = STRING_TO_JSVAL(str2);
                    } else {
                        str2 = JSVAL_TO_STRING(rval);
                        ok = (str = js_ValueToString(cx, lval)) != NULL;
                        if (!ok)
                            goto out;
                        sp[-2] = STRING_TO_JSVAL(str);
                    }
                    str = js_ConcatStrings(cx, str, str2);
                    if (!str) {
                        ok = JS_FALSE;
                        goto out;
                    }
                    sp--;
                    STORE_OPND(-1, STRING_TO_JSVAL(str));
                } else {
                    VALUE_TO_NUMBER(cx, lval, d);
                    VALUE_TO_NUMBER(cx, rval, d2);
                    d += d2;
                    sp--;
                    STORE_NUMBER(cx, -1, d);
                }
            }
          END_CASE(JSOP_ADD)

#define BINARY_OP(OP)                                                         \
    JS_BEGIN_MACRO                                                            \
        FETCH_NUMBER(cx, -1, d2);                                             \
        FETCH_NUMBER(cx, -2, d);                                              \
        d = d OP d2;                                                          \
        sp--;                                                                 \
        STORE_NUMBER(cx, -1, d);                                              \
    JS_END_MACRO

          BEGIN_CASE(JSOP_SUB)
            BINARY_OP(-);
          END_CASE(JSOP_SUB)

          BEGIN_CASE(JSOP_MUL)
            BINARY_OP(*);
          END_CASE(JSOP_MUL)

          BEGIN_CASE(JSOP_DIV)
            FETCH_NUMBER(cx, -1, d2);
            FETCH_NUMBER(cx, -2, d);
            sp--;
            if (d2 == 0) {
#ifdef XP_WIN
                /* XXX MSVC miscompiles such that (NaN == 0) */
                if (JSDOUBLE_IS_NaN(d2))
                    rval = DOUBLE_TO_JSVAL(rt->jsNaN);
                else
#endif
                if (d == 0 || JSDOUBLE_IS_NaN(d))
                    rval = DOUBLE_TO_JSVAL(rt->jsNaN);
                else if ((JSDOUBLE_HI32(d) ^ JSDOUBLE_HI32(d2)) >> 31)
                    rval = DOUBLE_TO_JSVAL(rt->jsNegativeInfinity);
                else
                    rval = DOUBLE_TO_JSVAL(rt->jsPositiveInfinity);
                STORE_OPND(-1, rval);
            } else {
                d /= d2;
                STORE_NUMBER(cx, -1, d);
            }
          END_CASE(JSOP_DIV)

          BEGIN_CASE(JSOP_MOD)
            FETCH_NUMBER(cx, -1, d2);
            FETCH_NUMBER(cx, -2, d);
            sp--;
            if (d2 == 0) {
                STORE_OPND(-1, DOUBLE_TO_JSVAL(rt->jsNaN));
            } else {
#ifdef XP_WIN
              /* Workaround MS fmod bug where 42 % (1/0) => NaN, not 42. */
              if (!(JSDOUBLE_IS_FINITE(d) && JSDOUBLE_IS_INFINITE(d2)))
#endif
                d = fmod(d, d2);
                STORE_NUMBER(cx, -1, d);
            }
          END_CASE(JSOP_MOD)

          BEGIN_CASE(JSOP_NOT)
            POP_BOOLEAN(cx, rval, cond);
            PUSH_OPND(BOOLEAN_TO_JSVAL(!cond));
          END_CASE(JSOP_NOT)

          BEGIN_CASE(JSOP_BITNOT)
            FETCH_INT(cx, -1, i);
            i = ~i;
            STORE_INT(cx, -1, i);
          END_CASE(JSOP_BITNOT)

          BEGIN_CASE(JSOP_NEG)
            /*
             * Optimize the case of an int-tagged operand by noting that
             * INT_FITS_IN_JSVAL(i) => INT_FITS_IN_JSVAL(-i) unless i is 0
             * when -i is the negative zero which is jsdouble.
             */
            rval = FETCH_OPND(-1);
            if (JSVAL_IS_INT(rval) && (i = JSVAL_TO_INT(rval)) != 0) {
                i = -i;
                JS_ASSERT(INT_FITS_IN_JSVAL(i));
                rval = INT_TO_JSVAL(i);
            } else {
                if (JSVAL_IS_DOUBLE(rval)) {
                    d = *JSVAL_TO_DOUBLE(rval);
                } else {
                    SAVE_SP_AND_PC(fp);
                    ok = js_ValueToNumber(cx, rval, &d);
                    if (!ok)
                        goto out;
                }
#ifdef HPUX
                /*
                 * Negation of a zero doesn't produce a negative
                 * zero on HPUX. Perform the operation by bit
                 * twiddling.
                 */
                JSDOUBLE_HI32(d) ^= JSDOUBLE_HI32_SIGNBIT;
#else
                d = -d;
#endif
                ok = js_NewNumberValue(cx, d, &rval);
                if (!ok)
                    goto out;
            }
            STORE_OPND(-1, rval);
          END_CASE(JSOP_NEG)

          BEGIN_CASE(JSOP_POS)
            rval = FETCH_OPND(-1);
            if (!JSVAL_IS_NUMBER(rval)) {
                SAVE_SP_AND_PC(fp);
                ok = js_ValueToNumber(cx, rval, &d);
                if (!ok)
                    goto out;
                ok = js_NewNumberValue(cx, d, &rval);
                if (!ok)
                    goto out;
                sp[-1] = rval;
            }
            sp[-1-depth] = (jsval)pc;
          END_CASE(JSOP_POS)

          BEGIN_CASE(JSOP_NEW)
            /* Get immediate argc and find the constructor function. */
            argc = GET_ARGC(pc);

          do_new:
            SAVE_SP_AND_PC(fp);
            vp = sp - (2 + argc);
            JS_ASSERT(vp >= fp->spbase);

            ok = js_InvokeConstructor(cx, vp, argc);
            if (!ok)
                goto out;
            RESTORE_SP(fp);
            LOAD_BRANCH_CALLBACK(cx);
            LOAD_INTERRUPT_HANDLER(rt);
            obj = JSVAL_TO_OBJECT(*vp);
            len = js_CodeSpec[op].length;
            DO_NEXT_OP(len);

          BEGIN_CASE(JSOP_DELNAME)
            atom = GET_ATOM(cx, script, pc);
            id   = ATOM_TO_JSID(atom);

            SAVE_SP_AND_PC(fp);
            ok = js_FindProperty(cx, id, &obj, &obj2, &prop);
            if (!ok)
                goto out;

            /* ECMA says to return true if name is undefined or inherited. */
            rval = JSVAL_TRUE;
            if (prop) {
                OBJ_DROP_PROPERTY(cx, obj2, prop);
                ok = OBJ_DELETE_PROPERTY(cx, obj, id, &rval);
                if (!ok)
                    goto out;
            }
            PUSH_OPND(rval);
          END_CASE(JSOP_DELNAME)

          BEGIN_CASE(JSOP_DELPROP)
            atom = GET_ATOM(cx, script, pc);
            id   = ATOM_TO_JSID(atom);
            PROPERTY_OP(-1, ok = OBJ_DELETE_PROPERTY(cx, obj, id, &rval));
            STORE_OPND(-1, rval);
          END_CASE(JSOP_DELPROP)

          BEGIN_CASE(JSOP_DELELEM)
            ELEMENT_OP(-1, ok = OBJ_DELETE_PROPERTY(cx, obj, id, &rval));
            sp--;
            STORE_OPND(-1, rval);
          END_CASE(JSOP_DELELEM)

          BEGIN_CASE(JSOP_TYPEOFEXPR)
          BEGIN_CASE(JSOP_TYPEOF)
            rval = FETCH_OPND(-1);
            SAVE_SP_AND_PC(fp);
            type = JS_TypeOfValue(cx, rval);
            atom = rt->atomState.typeAtoms[type];
            STORE_OPND(-1, ATOM_KEY(atom));
          END_CASE(JSOP_TYPEOF)

          BEGIN_CASE(JSOP_VOID)
            (void) POP_OPND();
            PUSH_OPND(JSVAL_VOID);
          END_CASE(JSOP_VOID)

          BEGIN_CASE(JSOP_INCNAME)
          BEGIN_CASE(JSOP_DECNAME)
          BEGIN_CASE(JSOP_NAMEINC)
          BEGIN_CASE(JSOP_NAMEDEC)
            atom = GET_ATOM(cx, script, pc);
            id   = ATOM_TO_JSID(atom);

            SAVE_SP_AND_PC(fp);
            ok = js_FindProperty(cx, id, &obj, &obj2, &prop);
            if (!ok)
                goto out;
            if (!prop)
                goto atom_not_defined;

            OBJ_DROP_PROPERTY(cx, obj2, prop);
            lval = OBJECT_TO_JSVAL(obj);
            i = 0;
            goto do_incop;

          BEGIN_CASE(JSOP_INCPROP)
          BEGIN_CASE(JSOP_DECPROP)
          BEGIN_CASE(JSOP_PROPINC)
          BEGIN_CASE(JSOP_PROPDEC)
            atom = GET_ATOM(cx, script, pc);
            id   = ATOM_TO_JSID(atom);
            lval = FETCH_OPND(-1);
            i = -1;
            goto do_incop;

          BEGIN_CASE(JSOP_INCELEM)
          BEGIN_CASE(JSOP_DECELEM)
          BEGIN_CASE(JSOP_ELEMINC)
          BEGIN_CASE(JSOP_ELEMDEC)
            FETCH_ELEMENT_ID(-1, id);
            lval = FETCH_OPND(-2);
            i = -2;

          do_incop:
          {
            const JSCodeSpec *cs;

            VALUE_TO_OBJECT(cx, lval, obj);
            if (i < 0)
                STORE_OPND(i, OBJECT_TO_JSVAL(obj));
            CHECK_ELEMENT_ID(obj, id);

            /* The operand must contain a number. */
            SAVE_SP_AND_PC(fp);
            CACHED_GET(OBJ_GET_PROPERTY(cx, obj, id, &rval));
            if (!ok)
                goto out;

            /* Preload for use in the if/else immediately below. */
            cs = &js_CodeSpec[op];

            /* The expression result goes in rtmp, the updated value in rval. */
            if (JSVAL_IS_INT(rval) &&
                rval != INT_TO_JSVAL(JSVAL_INT_MIN) &&
                rval != INT_TO_JSVAL(JSVAL_INT_MAX)) {
                if (cs->format & JOF_POST) {
                    rtmp = rval;
                    (cs->format & JOF_INC) ? (rval += 2) : (rval -= 2);
                } else {
                    (cs->format & JOF_INC) ? (rval += 2) : (rval -= 2);
                    rtmp = rval;
                }
            } else {

/*
 * Initially, rval contains the value to increment or decrement, which is not
 * yet converted.  As above, the expression result goes in rtmp, the updated
 * value goes in rval.  Our caller must set vp to point at a GC-rooted jsval
 * in which we home rtmp, to protect it from GC in case the unconverted rval
 * is not a number.
 */
#define NONINT_INCREMENT_OP_MIDDLE()                                          \
    JS_BEGIN_MACRO                                                            \
        VALUE_TO_NUMBER(cx, rval, d);                                         \
        if (cs->format & JOF_POST) {                                          \
            rtmp = rval;                                                      \
            if (!JSVAL_IS_NUMBER(rtmp)) {                                     \
                ok = js_NewNumberValue(cx, d, &rtmp);                         \
                if (!ok)                                                      \
                    goto out;                                                 \
            }                                                                 \
            *vp = rtmp;                                                       \
            (cs->format & JOF_INC) ? d++ : d--;                               \
            ok = js_NewNumberValue(cx, d, &rval);                             \
        } else {                                                              \
            (cs->format & JOF_INC) ? ++d : --d;                               \
            ok = js_NewNumberValue(cx, d, &rval);                             \
            rtmp = rval;                                                      \
        }                                                                     \
        if (!ok)                                                              \
            goto out;                                                         \
    JS_END_MACRO

                if (cs->format & JOF_POST) {
                    /*
                     * We must push early to protect the postfix increment
                     * or decrement result, if converted to a jsdouble from
                     * a non-number value, from GC nesting in the setter.
                     */
                    vp = sp;
                    PUSH(JSVAL_VOID);
                    SAVE_SP(fp);
                    --i;
                }
#ifdef __GNUC__
                else vp = NULL; /* suppress bogus gcc warnings */
#endif

                NONINT_INCREMENT_OP_MIDDLE();
            }

            fp->flags |= JSFRAME_ASSIGNING;
            CACHED_SET(OBJ_SET_PROPERTY(cx, obj, id, &rval));
            fp->flags &= ~JSFRAME_ASSIGNING;
            if (!ok)
                goto out;
            sp += i;
            PUSH_OPND(rtmp);
            len = js_CodeSpec[op].length;
            DO_NEXT_OP(len);
          }

/* NB: This macro doesn't use JS_BEGIN_MACRO/JS_END_MACRO around its body. */
#define FAST_INCREMENT_OP(SLOT,COUNT,BASE,PRE,OPEQ,MINMAX)                    \
    slot = SLOT;                                                              \
    JS_ASSERT(slot < fp->fun->COUNT);                                         \
    vp = fp->BASE + slot;                                                     \
    rval = *vp;                                                               \
    if (!JSVAL_IS_INT(rval) || rval == INT_TO_JSVAL(JSVAL_INT_##MINMAX))      \
        goto do_nonint_fast_incop;                                            \
    PRE = rval;                                                               \
    rval OPEQ 2;                                                              \
    *vp = rval;                                                               \
    PUSH_OPND(PRE);                                                           \
    goto end_nonint_fast_incop

          BEGIN_CASE(JSOP_INCARG)
            FAST_INCREMENT_OP(GET_ARGNO(pc), nargs, argv, rval, +=, MAX);
          BEGIN_CASE(JSOP_DECARG)
            FAST_INCREMENT_OP(GET_ARGNO(pc), nargs, argv, rval, -=, MIN);
          BEGIN_CASE(JSOP_ARGINC)
            FAST_INCREMENT_OP(GET_ARGNO(pc), nargs, argv, rtmp, +=, MAX);
          BEGIN_CASE(JSOP_ARGDEC)
            FAST_INCREMENT_OP(GET_ARGNO(pc), nargs, argv, rtmp, -=, MIN);

          BEGIN_CASE(JSOP_INCVAR)
            FAST_INCREMENT_OP(GET_VARNO(pc), u.i.nvars, vars, rval, +=, MAX);
          BEGIN_CASE(JSOP_DECVAR)
            FAST_INCREMENT_OP(GET_VARNO(pc), u.i.nvars, vars, rval, -=, MIN);
          BEGIN_CASE(JSOP_VARINC)
            FAST_INCREMENT_OP(GET_VARNO(pc), u.i.nvars, vars, rtmp, +=, MAX);
          BEGIN_CASE(JSOP_VARDEC)
            FAST_INCREMENT_OP(GET_VARNO(pc), u.i.nvars, vars, rtmp, -=, MIN);

          end_nonint_fast_incop:
            len = JSOP_INCARG_LENGTH;   /* all fast incops are same length */
            DO_NEXT_OP(len);

#undef FAST_INCREMENT_OP

          do_nonint_fast_incop:
          {
            const JSCodeSpec *cs = &js_CodeSpec[op];

            NONINT_INCREMENT_OP_MIDDLE();
            *vp = rval;
            PUSH_OPND(rtmp);
            len = cs->length;
            DO_NEXT_OP(len);
          }

/* NB: This macro doesn't use JS_BEGIN_MACRO/JS_END_MACRO around its body. */
#define FAST_GLOBAL_INCREMENT_OP(SLOWOP,PRE,OPEQ,MINMAX)                      \
    slot = GET_VARNO(pc);                                                     \
    JS_ASSERT(slot < fp->nvars);                                              \
    lval = fp->vars[slot];                                                    \
    if (JSVAL_IS_NULL(lval)) {                                                \
        op = SLOWOP;                                                          \
        DO_OP();                                                              \
    }                                                                         \
    slot = JSVAL_TO_INT(lval);                                                \
    obj = fp->varobj;                                                         \
    rval = OBJ_GET_SLOT(cx, obj, slot);                                       \
    if (!JSVAL_IS_INT(rval) || rval == INT_TO_JSVAL(JSVAL_INT_##MINMAX))      \
        goto do_nonint_fast_global_incop;                                     \
    PRE = rval;                                                               \
    rval OPEQ 2;                                                              \
    OBJ_SET_SLOT(cx, obj, slot, rval);                                        \
    PUSH_OPND(PRE);                                                           \
    goto end_nonint_fast_global_incop

          BEGIN_CASE(JSOP_INCGVAR)
            FAST_GLOBAL_INCREMENT_OP(JSOP_INCNAME, rval, +=, MAX);
          BEGIN_CASE(JSOP_DECGVAR)
            FAST_GLOBAL_INCREMENT_OP(JSOP_DECNAME, rval, -=, MIN);
          BEGIN_CASE(JSOP_GVARINC)
            FAST_GLOBAL_INCREMENT_OP(JSOP_NAMEINC, rtmp, +=, MAX);
          BEGIN_CASE(JSOP_GVARDEC)
            FAST_GLOBAL_INCREMENT_OP(JSOP_NAMEDEC, rtmp, -=, MIN);

          end_nonint_fast_global_incop:
            len = JSOP_INCGVAR_LENGTH;  /* all gvar incops are same length */
            JS_ASSERT(len == js_CodeSpec[op].length);
            DO_NEXT_OP(len);

#undef FAST_GLOBAL_INCREMENT_OP

          do_nonint_fast_global_incop:
          {
            const JSCodeSpec *cs = &js_CodeSpec[op];

            vp = sp++;
            SAVE_SP(fp);
            NONINT_INCREMENT_OP_MIDDLE();
            OBJ_SET_SLOT(cx, obj, slot, rval);
            STORE_OPND(-1, rtmp);
            len = cs->length;
            DO_NEXT_OP(len);
          }

          BEGIN_CASE(JSOP_GETPROP)
          BEGIN_CASE(JSOP_GETXPROP)
            /* Get an immediate atom naming the property. */
            atom = GET_ATOM(cx, script, pc);
            lval = FETCH_OPND(-1);
            if (JSVAL_IS_STRING(lval) &&
                atom == cx->runtime->atomState.lengthAtom) {
                rval = INT_TO_JSVAL(JSSTRING_LENGTH(JSVAL_TO_STRING(lval)));
                obj = NULL;
            } else {
                id = ATOM_TO_JSID(atom);
                VALUE_TO_OBJECT(cx, lval, obj);
                STORE_OPND(-1, OBJECT_TO_JSVAL(obj));
                SAVE_SP_AND_PC(fp);
                CACHED_GET(OBJ_GET_PROPERTY(cx, obj, id, &rval));
                if (!ok)
                    goto out;
            }
            STORE_OPND(-1, rval);
          END_CASE(JSOP_GETPROP)

          BEGIN_CASE(JSOP_SETPROP)
            /* Pop the right-hand side into rval for OBJ_SET_PROPERTY. */
            rval = FETCH_OPND(-1);

            /* Get an immediate atom naming the property. */
            atom = GET_ATOM(cx, script, pc);
            id   = ATOM_TO_JSID(atom);
            PROPERTY_OP(-2, CACHED_SET(OBJ_SET_PROPERTY(cx, obj, id, &rval)));
            sp--;
            STORE_OPND(-1, rval);
            obj = NULL;
          END_CASE(JSOP_SETPROP)

          BEGIN_CASE(JSOP_GETELEM)
          BEGIN_CASE(JSOP_GETXELEM)
            ELEMENT_OP(-1, CACHED_GET(OBJ_GET_PROPERTY(cx, obj, id, &rval)));
            sp--;
            STORE_OPND(-1, rval);
          END_CASE(JSOP_GETELEM)

          BEGIN_CASE(JSOP_SETELEM)
            rval = FETCH_OPND(-1);
            ELEMENT_OP(-2, CACHED_SET(OBJ_SET_PROPERTY(cx, obj, id, &rval)));
            sp -= 2;
            STORE_OPND(-1, rval);
            obj = NULL;
          END_CASE(JSOP_SETELEM)

          BEGIN_CASE(JSOP_ENUMELEM)
            /* Funky: the value to set is under the [obj, id] pair. */
            FETCH_ELEMENT_ID(-1, id);
            FETCH_OBJECT(cx, -2, lval, obj);
            CHECK_ELEMENT_ID(obj, id);
            rval = FETCH_OPND(-3);
            SAVE_SP_AND_PC(fp);
            ok = OBJ_SET_PROPERTY(cx, obj, id, &rval);
            if (!ok)
                goto out;
            sp -= 3;
          END_CASE(JSOP_ENUMELEM)

/*
 * LAZY_ARGS_THISP allows the JSOP_ARGSUB bytecode to defer creation of the
 * arguments object until it is truly needed.  JSOP_ARGSUB optimizes away
 * arguments objects when the only uses of the 'arguments' parameter are to
 * fetch individual actual parameters.  But if such a use were then invoked,
 * e.g., arguments[i](), the 'this' parameter would and must bind to the
 * caller's arguments object.  So JSOP_ARGSUB sets obj to LAZY_ARGS_THISP.
 */
#define LAZY_ARGS_THISP ((JSObject *) JSVAL_VOID)

          BEGIN_CASE(JSOP_PUSHOBJ)
            if (obj == LAZY_ARGS_THISP && !(obj = js_GetArgsObject(cx, fp))) {
                ok = JS_FALSE;
                goto out;
            }
            PUSH_OPND(OBJECT_TO_JSVAL(obj));
          END_CASE(JSOP_PUSHOBJ)

          BEGIN_CASE(JSOP_CALL)
          BEGIN_CASE(JSOP_EVAL)
            argc = GET_ARGC(pc);
            vp = sp - (argc + 2);
            lval = *vp;
            SAVE_SP_AND_PC(fp);
            if (VALUE_IS_FUNCTION(cx, lval) &&
                (obj = JSVAL_TO_OBJECT(lval),
                 fun = (JSFunction *) JS_GetPrivate(cx, obj),
                 FUN_INTERPRETED(fun)))
          /* inline_call: */
            {
                uintN nframeslots, nvars, nslots, missing;
                JSArena *a;
                jsuword avail, nbytes;
                JSBool overflow;
                void *newmark;
                jsval *rvp;
                JSInlineFrame *newifp;
                JSInterpreterHook hook;

                /* Restrict recursion of lightweight functions. */
                if (inlineCallCount == MAX_INLINE_CALL_COUNT) {
                    JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL,
                                         JSMSG_OVER_RECURSED);
                    ok = JS_FALSE;
                    goto out;
                }

                /* Compute the total number of stack slots needed for fun. */
                nframeslots = JS_HOWMANY(sizeof(JSInlineFrame), sizeof(jsval));
                nvars = fun->u.i.nvars;
                script = fun->u.i.script;
                depth = (jsint) script->depth;
                nslots = nframeslots + nvars + 2 * depth;

                /* Allocate missing expected args adjacent to actual args. */
                missing = (fun->nargs > argc) ? fun->nargs - argc : 0;
                a = cx->stackPool.current;
                avail = a->avail;
                newmark = (void *) avail;
                if (missing) {
                    newsp = sp + missing;
                    overflow = (jsuword) newsp > a->limit;
                    if (overflow)
                        nslots += 2 + argc + missing;
                    else if ((jsuword) newsp > avail)
                        avail = a->avail = (jsuword) newsp;
                }
#ifdef __GNUC__
                else overflow = JS_FALSE;   /* suppress bogus gcc warnings */
#endif

                /* Allocate the inline frame with its vars and operand slots. */
                newsp = (jsval *) avail;
                nbytes = nslots * sizeof(jsval);
                avail += nbytes;
                if (avail <= a->limit) {
                    a->avail = avail;
                } else {
                    JS_ARENA_ALLOCATE_CAST(newsp, jsval *, &cx->stackPool,
                                           nbytes);
                    if (!newsp) {
                        JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL,
                                             JSMSG_STACK_OVERFLOW,
                                             (fp && fp->fun)
                                             ? JS_GetFunctionName(fp->fun)
                                             : "script");
                        goto bad_inline_call;
                    }
                }

                /* Move args if missing overflow arena a, push missing args. */
                rvp = vp;
                if (missing) {
                    if (overflow) {
                        memcpy(newsp, vp, (2 + argc) * sizeof(jsval));
                        vp = newsp;
                        sp = vp + 2 + argc;
                        newsp = sp + missing;
                    }
                    do {
                        PUSH(JSVAL_VOID);
                    } while (--missing != 0);
                }

                /* Claim space for the stack frame and initialize it. */
                newifp = (JSInlineFrame *) newsp;
                newsp += nframeslots;
                newifp->frame.callobj = NULL;
                newifp->frame.argsobj = NULL;
                newifp->frame.varobj = NULL;
                newifp->frame.script = script;
                newifp->frame.fun = fun;
                newifp->frame.argc = argc;
                newifp->frame.argv = vp + 2;
                newifp->frame.rval = JSVAL_VOID;
                newifp->frame.nvars = nvars;
                newifp->frame.vars = newsp;
                newifp->frame.down = fp;
                newifp->frame.annotation = NULL;
                newifp->frame.scopeChain = parent = OBJ_GET_PARENT(cx, obj);
                newifp->frame.sharpDepth = 0;
                newifp->frame.sharpArray = NULL;
                newifp->frame.flags = 0;
                newifp->frame.dormantNext = NULL;
                newifp->frame.xmlNamespace = NULL;
                newifp->frame.blockChain = NULL;
                newifp->rvp = rvp;
                newifp->mark = newmark;

                /* Compute the 'this' parameter now that argv is set. */
                if (!JSVAL_IS_OBJECT(vp[1])) {
                    PRIMITIVE_TO_OBJECT(cx, vp[1], obj2);
                    if (!obj2)
                        goto bad_inline_call;
                    vp[1] = OBJECT_TO_JSVAL(obj2);
                }
                newifp->frame.thisp =
                    js_ComputeThis(cx,
                                   JSFUN_BOUND_METHOD_TEST(fun->flags)
                                   ? parent
                                   : JSVAL_TO_OBJECT(vp[1]),
                                   newifp->frame.argv);
                if (!newifp->frame.thisp)
                    goto bad_inline_call;
#ifdef DUMP_CALL_TABLE
                LogCall(cx, *vp, argc, vp + 2);
#endif

                /* Push void to initialize local variables. */
                sp = newsp;
                while (nvars--)
                    PUSH(JSVAL_VOID);
                sp += depth;
                newifp->frame.spbase = sp;
                SAVE_SP(&newifp->frame);

                /* Call the debugger hook if present. */
                hook = rt->callHook;
                if (hook) {
                    newifp->frame.pc = NULL;
                    newifp->hookData = hook(cx, &newifp->frame, JS_TRUE, 0,
                                            rt->callHookData);
                    LOAD_INTERRUPT_HANDLER(rt);
                } else {
                    newifp->hookData = NULL;
                }

                /* Scope with a call object parented by the callee's parent. */
                if (JSFUN_HEAVYWEIGHT_TEST(fun->flags) &&
                    !js_GetCallObject(cx, &newifp->frame, parent)) {
                    goto bad_inline_call;
                }

                /* Switch to new version if currentVersion wasn't overridden. */
                newifp->callerVersion = cx->version;
                if (JS_LIKELY(cx->version == currentVersion)) {
                    currentVersion = script->version;
                    if (currentVersion != cx->version)
                        js_SetVersion(cx, currentVersion);
                }

                /* Push the frame and set interpreter registers. */
                cx->fp = fp = &newifp->frame;
                pc = script->code;
#ifndef JS_THREADED_INTERP
                endpc = pc + script->length;
#endif
                obj = NULL;
                inlineCallCount++;
                JS_RUNTIME_METER(rt, inlineCalls);

                /* Load first opcode and dispatch it (safe since JSOP_STOP). */
                op = *pc;
                DO_OP();

              bad_inline_call:
                RESTORE_SP(fp);
                JS_ASSERT(fp->pc == pc);
                script = fp->script;
                depth = (jsint) script->depth;
                js_FreeRawStack(cx, newmark);
                ok = JS_FALSE;
                goto out;
            }

            ok = js_Invoke(cx, argc, 0);
            RESTORE_SP(fp);
            LOAD_BRANCH_CALLBACK(cx);
            LOAD_INTERRUPT_HANDLER(rt);
            if (!ok)
                goto out;
            JS_RUNTIME_METER(rt, nonInlineCalls);
#if JS_HAS_LVALUE_RETURN
            if (cx->rval2set) {
                /*
                 * Use the stack depth we didn't claim in our budget, but that
                 * we know is there on account of [fun, this] already having
                 * been pushed, at a minimum (if no args).  Those two slots
                 * have been popped and [rval] has been pushed, which leaves
                 * one more slot for rval2 before we might overflow.
                 *
                 * NB: rval2 must be the property identifier, and rval the
                 * object from which to get the property.  The pair form an
                 * ECMA "reference type", which can be used on the right- or
                 * left-hand side of assignment ops.  Note well: only native
                 * methods can return reference types.  See JSOP_SETCALL just
                 * below for the left-hand-side case.
                 */
                PUSH_OPND(cx->rval2);
                ELEMENT_OP(-1, ok = OBJ_GET_PROPERTY(cx, obj, id, &rval));

                sp--;
                STORE_OPND(-1, rval);
                cx->rval2set = JS_FALSE;
            }
#endif /* JS_HAS_LVALUE_RETURN */
            obj = NULL;
          END_CASE(JSOP_CALL)

#if JS_HAS_LVALUE_RETURN
          BEGIN_CASE(JSOP_SETCALL)
            argc = GET_ARGC(pc);
            SAVE_SP_AND_PC(fp);
            ok = js_Invoke(cx, argc, 0);
            RESTORE_SP(fp);
            LOAD_BRANCH_CALLBACK(cx);
            LOAD_INTERRUPT_HANDLER(rt);
            if (!ok)
                goto out;
            if (!cx->rval2set) {
                JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL,
                                     JSMSG_BAD_LEFTSIDE_OF_ASS);
                ok = JS_FALSE;
                goto out;
            }
            PUSH_OPND(cx->rval2);
            cx->rval2set = JS_FALSE;
            obj = NULL;
          END_CASE(JSOP_SETCALL)
#endif

          BEGIN_CASE(JSOP_NAME)
            atom = GET_ATOM(cx, script, pc);
            id   = ATOM_TO_JSID(atom);

            SAVE_SP_AND_PC(fp);
            ok = js_FindProperty(cx, id, &obj, &obj2, &prop);
            if (!ok)
                goto out;
            if (!prop) {
                /* Kludge to allow (typeof foo == "undefined") tests. */
                len = JSOP_NAME_LENGTH;
                endpc = script->code + script->length;
                for (pc2 = pc + len; pc2 < endpc; pc2++) {
                    op2 = (JSOp)*pc2;
                    if (op2 == JSOP_TYPEOF) {
                        PUSH_OPND(JSVAL_VOID);
                        DO_NEXT_OP(len);
                    }
                    if (op2 != JSOP_GROUP)
                        break;
                }
                goto atom_not_defined;
            }

            /* Take the slow path if prop was not found in a native object. */
            if (!OBJ_IS_NATIVE(obj) || !OBJ_IS_NATIVE(obj2)) {
                OBJ_DROP_PROPERTY(cx, obj2, prop);
                ok = OBJ_GET_PROPERTY(cx, obj, id, &rval);
                if (!ok)
                    goto out;
            } else {
                sprop = (JSScopeProperty *)prop;
                NATIVE_GET(cx, obj, obj2, sprop, &rval);
                OBJ_DROP_PROPERTY(cx, obj2, prop);
            }
            PUSH_OPND(rval);
          END_CASE(JSOP_NAME)

          BEGIN_CASE(JSOP_UINT16)
            i = (jsint) GET_ATOM_INDEX(pc);
            rval = INT_TO_JSVAL(i);
            PUSH_OPND(rval);
            obj = NULL;
          END_CASE(JSOP_UINT16)

          BEGIN_CASE(JSOP_UINT24)
            i = (jsint) GET_LITERAL_INDEX(pc);
            rval = INT_TO_JSVAL(i);
            PUSH_OPND(rval);
          END_CASE(JSOP_UINT24)

          BEGIN_CASE(JSOP_LITERAL)
            atomIndex = GET_LITERAL_INDEX(pc);
            atom = js_GetAtom(cx, &script->atomMap, atomIndex);
            PUSH_OPND(ATOM_KEY(atom));
            obj = NULL;
          END_CASE(JSOP_LITERAL)

          BEGIN_CASE(JSOP_FINDNAME)
            atomIndex = GET_LITERAL_INDEX(pc);
            atom = js_GetAtom(cx, &script->atomMap, atomIndex);
            SAVE_SP_AND_PC(fp);
            obj = js_FindIdentifierBase(cx, ATOM_TO_JSID(atom));
            if (!obj) {
                ok = JS_FALSE;
                goto out;
            }
            PUSH_OPND(OBJECT_TO_JSVAL(obj));
            PUSH_OPND(ATOM_KEY(atom));
          END_CASE(JSOP_FINDNAME)

          BEGIN_CASE(JSOP_LITOPX)
            /*
             * Load atomIndex, which is used by code at each do_JSOP_* label.
             *
             * Also set pc2 to point at the bytecode extended by this prefix
             * to have a leading 24 bit atomIndex, instead of the unextended
             * 16-bit atomIndex that normally comes after op.  This enables
             * JOF_INDEXCONST format ops (which have multiple immediates) to
             * collect their other immediate via GET_VARNO(pc2) or similar.
             *
             * Finally, load op and, if threading, adjust pc so that it will
             * be advanced properly at the end of op's case by DO_NEXT_OP.
             */
            atomIndex = GET_LITERAL_INDEX(pc);
            pc2 = pc + 1 + LITERAL_INDEX_LEN;
            op = *pc2;
            pc += JSOP_LITOPX_LENGTH - (1 + ATOM_INDEX_LEN);
#ifndef JS_THREADED_INTERP
            len = js_CodeSpec[op].length;
#endif
            switch (op) {
              case JSOP_ANONFUNOBJ:   goto do_JSOP_ANONFUNOBJ;
              case JSOP_BINDNAME:     goto do_JSOP_BINDNAME;
              case JSOP_CLOSURE:      goto do_JSOP_CLOSURE;
              case JSOP_DEFCONST:     goto do_JSOP_DEFCONST;
              case JSOP_DEFFUN:       goto do_JSOP_DEFFUN;
              case JSOP_DEFLOCALFUN:  goto do_JSOP_DEFLOCALFUN;
              case JSOP_DEFVAR:       goto do_JSOP_DEFVAR;
#if JS_HAS_EXPORT_IMPORT
              case JSOP_EXPORTNAME:   goto do_JSOP_EXPORTNAME;
#endif
#if JS_HAS_XML_SUPPORT
              case JSOP_GETMETHOD:    goto do_JSOP_GETMETHOD;
              case JSOP_SETMETHOD:    goto do_JSOP_SETMETHOD;
#endif
              case JSOP_NAMEDFUNOBJ:  goto do_JSOP_NAMEDFUNOBJ;
              case JSOP_NUMBER:       goto do_JSOP_NUMBER;
              case JSOP_OBJECT:       goto do_JSOP_OBJECT;
#if JS_HAS_XML_SUPPORT
              case JSOP_QNAMECONST:   goto do_JSOP_QNAMECONST;
              case JSOP_QNAMEPART:    goto do_JSOP_QNAMEPART;
#endif
              case JSOP_REGEXP:       goto do_JSOP_REGEXP;
              case JSOP_SETCONST:     goto do_JSOP_SETCONST;
              case JSOP_STRING:       goto do_JSOP_STRING;
#if JS_HAS_XML_SUPPORT
              case JSOP_XMLCDATA:     goto do_JSOP_XMLCDATA;
              case JSOP_XMLCOMMENT:   goto do_JSOP_XMLCOMMENT;
              case JSOP_XMLOBJECT:    goto do_JSOP_XMLOBJECT;
              case JSOP_XMLPI:        goto do_JSOP_XMLPI;
#endif
              case JSOP_ENTERBLOCK:   goto do_JSOP_ENTERBLOCK;
              default:                JS_ASSERT(0);
            }
            /* NOTREACHED */

          BEGIN_CASE(JSOP_NUMBER)
          BEGIN_CASE(JSOP_STRING)
          BEGIN_CASE(JSOP_OBJECT)
            atomIndex = GET_ATOM_INDEX(pc);

          do_JSOP_NUMBER:
          do_JSOP_STRING:
          do_JSOP_OBJECT:
            atom = js_GetAtom(cx, &script->atomMap, atomIndex);
            PUSH_OPND(ATOM_KEY(atom));
            obj = NULL;
          END_CASE(JSOP_NUMBER)

          BEGIN_LITOPX_CASE(JSOP_REGEXP, 0)
          {
            JSRegExp *re;
            JSObject *funobj;

            /*
             * Push a regexp object for the atom mapped by the bytecode at pc,
             * cloning the literal's regexp object if necessary, to simulate in
             * the pre-compile/execute-later case what ECMA specifies for the
             * compile-and-go case: that scanning each regexp literal creates
             * a single corresponding RegExp object.
             *
             * To support pre-compilation transparently, we must handle the
             * case where a regexp object literal is used in a different global
             * at execution time from the global with which it was scanned at
             * compile time.  We do this by re-wrapping the JSRegExp private
             * data struct with a cloned object having the right prototype and
             * parent, and having its own lastIndex property value storage.
             *
             * Unlike JSOP_DEFFUN and other prolog bytecodes that may clone
             * literal objects, we don't want to pay a script prolog execution
             * price for all regexp literals in a script (many may not be used
             * by a particular execution of that script, depending on control
             * flow), so we initialize lazily here.
             *
             * XXX This code is specific to regular expression objects.  If we
             * need a similar op for other kinds of object literals, we should
             * push cloning down under JSObjectOps and reuse code here.
             */
            JS_ASSERT(ATOM_IS_OBJECT(atom));
            obj = ATOM_TO_OBJECT(atom);
            JS_ASSERT(OBJ_GET_CLASS(cx, obj) == &js_RegExpClass);

            re = (JSRegExp *) JS_GetPrivate(cx, obj);
            slot = re->cloneIndex;
            if (fp->fun) {
                /*
                 * We're in function code, not global or eval code (in eval
                 * code, JSOP_REGEXP is never emitted).  The code generator
                 * recorded in fp->fun->nregexps the number of re->cloneIndex
                 * slots that it reserved in the cloned funobj.
                 */
                funobj = JSVAL_TO_OBJECT(fp->argv[-2]);
                slot += JSCLASS_RESERVED_SLOTS(&js_FunctionClass);
                if (!JS_GetReservedSlot(cx, funobj, slot, &rval))
                    return JS_FALSE;
                if (JSVAL_IS_VOID(rval))
                    rval = JSVAL_NULL;
            } else {
                /*
                 * We're in global code.  The code generator already arranged
                 * via script->numGlobalVars to reserve a global variable slot
                 * at cloneIndex.  All global variable slots are initialized
                 * to null, not void, for faster testing in JSOP_*GVAR cases.
                 */
                rval = fp->vars[slot];
#ifdef __GNUC__
                funobj = NULL;  /* suppress bogus gcc warnings */
#endif
            }

            if (JSVAL_IS_NULL(rval)) {
                /* Compute the current global object in obj2. */
                obj2 = fp->scopeChain;
                while ((parent = OBJ_GET_PARENT(cx, obj2)) != NULL)
                    obj2 = parent;

                /*
                 * We must home sp here, because either js_CloneRegExpObject
                 * or JS_SetReservedSlot could nest a last-ditch GC.  We home
                 * pc as well, in case js_CloneRegExpObject has to lookup the
                 * "RegExp" class in the global object, which could entail a
                 * JSNewResolveOp call.
                 */
                SAVE_SP_AND_PC(fp);

                /*
                 * If obj's parent is not obj2, we must clone obj so that it
                 * has the right parent, and therefore, the right prototype.
                 *
                 * Yes, this means we assume that the correct RegExp.prototype
                 * to which regexp instances (including literals) delegate can
                 * be distinguished solely by the instance's parent, which was
                 * set to the parent of the RegExp constructor function object
                 * when the instance was created.  In other words,
                 *
                 *   (/x/.__parent__ == RegExp.__parent__) implies
                 *   (/x/.__proto__ == RegExp.prototype)
                 *
                 * (unless you assign a different object to RegExp.prototype
                 * at runtime, in which case, ECMA doesn't specify operation,
                 * and you get what you deserve).
                 *
                 * This same coupling between instance parent and constructor
                 * parent turns up everywhere (see jsobj.c's FindClassObject,
                 * js_ConstructObject, and js_NewObject).  It's fundamental to
                 * the design of the language when you consider multiple global
                 * objects and separate compilation and execution, even though
                 * it is not specified fully in ECMA.
                 */
                if (OBJ_GET_PARENT(cx, obj) != obj2) {
                    obj = js_CloneRegExpObject(cx, obj, obj2);
                    if (!obj) {
                        ok = JS_FALSE;
                        goto out;
                    }
                }
                rval = OBJECT_TO_JSVAL(obj);

                /* Store the regexp object value in its cloneIndex slot. */
                if (fp->fun) {
                    if (!JS_SetReservedSlot(cx, funobj, slot, rval))
                        return JS_FALSE;
                } else {
                    fp->vars[slot] = rval;
                }
            }

            PUSH_OPND(rval);
            obj = NULL;
          }
          END_LITOPX_CASE(JSOP_REGEXP)

          BEGIN_CASE(JSOP_ZERO)
            PUSH_OPND(JSVAL_ZERO);
            obj = NULL;
          END_CASE(JSOP_ZERO)

          BEGIN_CASE(JSOP_ONE)
            PUSH_OPND(JSVAL_ONE);
            obj = NULL;
          END_CASE(JSOP_ONE)

          BEGIN_CASE(JSOP_NULL)
            PUSH_OPND(JSVAL_NULL);
            obj = NULL;
          END_CASE(JSOP_NULL)

          BEGIN_CASE(JSOP_THIS)
            obj = fp->thisp;
            clasp = OBJ_GET_CLASS(cx, obj);
            if (clasp->flags & JSCLASS_IS_EXTENDED) {
                JSExtendedClass *xclasp;

                xclasp = (JSExtendedClass *) clasp;
                if (xclasp->outerObject) {
                    obj = xclasp->outerObject(cx, obj);
                    if (!obj) {
                        ok = JS_FALSE;
                        goto out;
                    }
                }
            }

            PUSH_OPND(OBJECT_TO_JSVAL(obj));
            obj = NULL;
          END_CASE(JSOP_THIS)

          BEGIN_CASE(JSOP_FALSE)
            PUSH_OPND(JSVAL_FALSE);
            obj = NULL;
          END_CASE(JSOP_FALSE)

          BEGIN_CASE(JSOP_TRUE)
            PUSH_OPND(JSVAL_TRUE);
            obj = NULL;
          END_CASE(JSOP_TRUE)

          BEGIN_CASE(JSOP_TABLESWITCH)
            pc2 = pc;
            len = GET_JUMP_OFFSET(pc2);

            /*
             * ECMAv2+ forbids conversion of discriminant, so we will skip to
             * the default case if the discriminant isn't already an int jsval.
             * (This opcode is emitted only for dense jsint-domain switches.)
             */
            rval = POP_OPND();
            if (!JSVAL_IS_INT(rval))
                DO_NEXT_OP(len);
            i = JSVAL_TO_INT(rval);

            pc2 += JUMP_OFFSET_LEN;
            low = GET_JUMP_OFFSET(pc2);
            pc2 += JUMP_OFFSET_LEN;
            high = GET_JUMP_OFFSET(pc2);

            i -= low;
            if ((jsuint)i < (jsuint)(high - low + 1)) {
                pc2 += JUMP_OFFSET_LEN + JUMP_OFFSET_LEN * i;
                off = (jsint) GET_JUMP_OFFSET(pc2);
                if (off)
                    len = off;
            }
          END_VARLEN_CASE

          BEGIN_CASE(JSOP_LOOKUPSWITCH)
            lval = POP_OPND();
            pc2 = pc;
            len = GET_JUMP_OFFSET(pc2);

            if (!JSVAL_IS_NUMBER(lval) &&
                !JSVAL_IS_STRING(lval) &&
                !JSVAL_IS_BOOLEAN(lval)) {
                DO_NEXT_OP(len);
            }

            pc2 += JUMP_OFFSET_LEN;
            npairs = (jsint) GET_ATOM_INDEX(pc2);
            pc2 += ATOM_INDEX_LEN;

#define SEARCH_PAIRS(MATCH_CODE)                                              \
    while (npairs) {                                                          \
        atom = GET_ATOM(cx, script, pc2);                                     \
        rval = ATOM_KEY(atom);                                                \
        MATCH_CODE                                                            \
        if (match) {                                                          \
            pc2 += ATOM_INDEX_LEN;                                            \
            len = GET_JUMP_OFFSET(pc2);                                       \
            DO_NEXT_OP(len);                                                  \
        }                                                                     \
        pc2 += ATOM_INDEX_LEN + JUMP_OFFSET_LEN;                              \
        npairs--;                                                             \
    }
            if (JSVAL_IS_STRING(lval)) {
                str  = JSVAL_TO_STRING(lval);
                SEARCH_PAIRS(
                    match = (JSVAL_IS_STRING(rval) &&
                             ((str2 = JSVAL_TO_STRING(rval)) == str ||
                              js_EqualStrings(str2, str)));
                )
            } else if (JSVAL_IS_DOUBLE(lval)) {
                d = *JSVAL_TO_DOUBLE(lval);
                SEARCH_PAIRS(
                    match = (JSVAL_IS_DOUBLE(rval) &&
                             *JSVAL_TO_DOUBLE(rval) == d);
                )
            } else {
                SEARCH_PAIRS(
                    match = (lval == rval);
                )
            }
#undef SEARCH_PAIRS
          END_VARLEN_CASE

          BEGIN_CASE(JSOP_TABLESWITCHX)
            pc2 = pc;
            len = GET_JUMPX_OFFSET(pc2);

            /*
             * ECMAv2+ forbids conversion of discriminant, so we will skip to
             * the default case if the discriminant isn't already an int jsval.
             * (This opcode is emitted only for dense jsint-domain switches.)
             */
            rval = POP_OPND();
            if (!JSVAL_IS_INT(rval))
                DO_NEXT_OP(len);
            i = JSVAL_TO_INT(rval);

            pc2 += JUMPX_OFFSET_LEN;
            low = GET_JUMP_OFFSET(pc2);
            pc2 += JUMP_OFFSET_LEN;
            high = GET_JUMP_OFFSET(pc2);

            i -= low;
            if ((jsuint)i < (jsuint)(high - low + 1)) {
                pc2 += JUMP_OFFSET_LEN + JUMPX_OFFSET_LEN * i;
                off = (jsint) GET_JUMPX_OFFSET(pc2);
                if (off)
                    len = off;
            }
          END_VARLEN_CASE

          BEGIN_CASE(JSOP_LOOKUPSWITCHX)
            lval = POP_OPND();
            pc2 = pc;
            len = GET_JUMPX_OFFSET(pc2);

            if (!JSVAL_IS_NUMBER(lval) &&
                !JSVAL_IS_STRING(lval) &&
                !JSVAL_IS_BOOLEAN(lval)) {
                DO_NEXT_OP(len);
            }

            pc2 += JUMPX_OFFSET_LEN;
            npairs = (jsint) GET_ATOM_INDEX(pc2);
            pc2 += ATOM_INDEX_LEN;

#define SEARCH_EXTENDED_PAIRS(MATCH_CODE)                                     \
    while (npairs) {                                                          \
        atom = GET_ATOM(cx, script, pc2);                                     \
        rval = ATOM_KEY(atom);                                                \
        MATCH_CODE                                                            \
        if (match) {                                                          \
            pc2 += ATOM_INDEX_LEN;                                            \
            len = GET_JUMPX_OFFSET(pc2);                                      \
            DO_NEXT_OP(len);                                                  \
        }                                                                     \
        pc2 += ATOM_INDEX_LEN + JUMPX_OFFSET_LEN;                             \
        npairs--;                                                             \
    }
            if (JSVAL_IS_STRING(lval)) {
                str  = JSVAL_TO_STRING(lval);
                SEARCH_EXTENDED_PAIRS(
                    match = (JSVAL_IS_STRING(rval) &&
                             ((str2 = JSVAL_TO_STRING(rval)) == str ||
                              js_EqualStrings(str2, str)));
                )
            } else if (JSVAL_IS_DOUBLE(lval)) {
                d = *JSVAL_TO_DOUBLE(lval);
                SEARCH_EXTENDED_PAIRS(
                    match = (JSVAL_IS_DOUBLE(rval) &&
                             *JSVAL_TO_DOUBLE(rval) == d);
                )
            } else {
                SEARCH_EXTENDED_PAIRS(
                    match = (lval == rval);
                )
            }
#undef SEARCH_EXTENDED_PAIRS
          END_VARLEN_CASE

          EMPTY_CASE(JSOP_CONDSWITCH)

#if JS_HAS_EXPORT_IMPORT
          BEGIN_CASE(JSOP_EXPORTALL)
            obj = fp->varobj;
            SAVE_SP_AND_PC(fp);
            ida = JS_Enumerate(cx, obj);
            if (!ida) {
                ok = JS_FALSE;
            } else {
                for (i = 0, j = ida->length; i < j; i++) {
                    id = ida->vector[i];
                    ok = OBJ_LOOKUP_PROPERTY(cx, obj, id, &obj2, &prop);
                    if (!ok)
                        break;
                    if (!prop)
                        continue;
                    ok = OBJ_GET_ATTRIBUTES(cx, obj, id, prop, &attrs);
                    if (ok) {
                        attrs |= JSPROP_EXPORTED;
                        ok = OBJ_SET_ATTRIBUTES(cx, obj, id, prop, &attrs);
                    }
                    OBJ_DROP_PROPERTY(cx, obj2, prop);
                    if (!ok)
                        break;
                }
                JS_DestroyIdArray(cx, ida);
            }
          END_CASE(JSOP_EXPORTALL)

          BEGIN_LITOPX_CASE(JSOP_EXPORTNAME, 0)
            id   = ATOM_TO_JSID(atom);
            obj  = fp->varobj;
            SAVE_SP_AND_PC(fp);
            ok = OBJ_LOOKUP_PROPERTY(cx, obj, id, &obj2, &prop);
            if (!ok)
                goto out;
            if (!prop) {
                ok = OBJ_DEFINE_PROPERTY(cx, obj, id, JSVAL_VOID, NULL, NULL,
                                         JSPROP_EXPORTED, NULL);
            } else {
                ok = OBJ_GET_ATTRIBUTES(cx, obj, id, prop, &attrs);
                if (ok) {
                    attrs |= JSPROP_EXPORTED;
                    ok = OBJ_SET_ATTRIBUTES(cx, obj, id, prop, &attrs);
                }
                OBJ_DROP_PROPERTY(cx, obj2, prop);
            }
            if (!ok)
                goto out;
          END_LITOPX_CASE(JSOP_EXPORTNAME)

          BEGIN_CASE(JSOP_IMPORTALL)
            id = (jsid) JSVAL_VOID;
            PROPERTY_OP(-1, ok = ImportProperty(cx, obj, id));
            sp--;
          END_CASE(JSOP_IMPORTALL)

          BEGIN_CASE(JSOP_IMPORTPROP)
            /* Get an immediate atom naming the property. */
            atom = GET_ATOM(cx, script, pc);
            id   = ATOM_TO_JSID(atom);
            PROPERTY_OP(-1, ok = ImportProperty(cx, obj, id));
            sp--;
          END_CASE(JSOP_IMPORTPROP)

          BEGIN_CASE(JSOP_IMPORTELEM)
            ELEMENT_OP(-1, ok = ImportProperty(cx, obj, id));
            sp -= 2;
          END_CASE(JSOP_IMPORTELEM)
#endif /* JS_HAS_EXPORT_IMPORT */

          BEGIN_CASE(JSOP_TRAP)
            SAVE_SP_AND_PC(fp);
            switch (JS_HandleTrap(cx, script, pc, &rval)) {
              case JSTRAP_ERROR:
                ok = JS_FALSE;
                goto out;
              case JSTRAP_CONTINUE:
                JS_ASSERT(JSVAL_IS_INT(rval));
                op = (JSOp) JSVAL_TO_INT(rval);
                JS_ASSERT((uintN)op < (uintN)JSOP_LIMIT);
                LOAD_INTERRUPT_HANDLER(rt);
                DO_OP();
              case JSTRAP_RETURN:
                fp->rval = rval;
                goto out;
              case JSTRAP_THROW:
                cx->throwing = JS_TRUE;
                cx->exception = rval;
                ok = JS_FALSE;
                goto out;
              default:;
            }
            LOAD_INTERRUPT_HANDLER(rt);
          END_CASE(JSOP_TRAP)

          BEGIN_CASE(JSOP_ARGUMENTS)
            SAVE_SP_AND_PC(fp);
            ok = js_GetArgsValue(cx, fp, &rval);
            if (!ok)
                goto out;
            PUSH_OPND(rval);
            obj = NULL;
          END_CASE(JSOP_ARGUMENTS)

          BEGIN_CASE(JSOP_ARGSUB)
            id = INT_TO_JSID(GET_ARGNO(pc));
            SAVE_SP_AND_PC(fp);
            ok = js_GetArgsProperty(cx, fp, id, &obj, &rval);
            if (!ok)
                goto out;
            if (!obj) {
                /*
                 * If arguments was not overridden by eval('arguments = ...'),
                 * set obj to the magic cookie respected by JSOP_PUSHOBJ, just
                 * in case this bytecode is part of an 'arguments[i](j, k)' or
                 * similar such invocation sequence, where the function that
                 * is invoked expects its 'this' parameter to be the caller's
                 * arguments object.
                 */
                obj = LAZY_ARGS_THISP;
            }
            PUSH_OPND(rval);
          END_CASE(JSOP_ARGSUB)

#undef LAZY_ARGS_THISP

          BEGIN_CASE(JSOP_ARGCNT)
            id = ATOM_TO_JSID(rt->atomState.lengthAtom);
            SAVE_SP_AND_PC(fp);
            ok = js_GetArgsProperty(cx, fp, id, &obj, &rval);
            if (!ok)
                goto out;
            PUSH_OPND(rval);
          END_CASE(JSOP_ARGCNT)

          BEGIN_CASE(JSOP_GETARG)
            slot = GET_ARGNO(pc);
            JS_ASSERT(slot < fp->fun->nargs);
            PUSH_OPND(fp->argv[slot]);
            obj = NULL;
          END_CASE(JSOP_GETARG)

          BEGIN_CASE(JSOP_SETARG)
            slot = GET_ARGNO(pc);
            JS_ASSERT(slot < fp->fun->nargs);
            vp = &fp->argv[slot];
            GC_POKE(cx, *vp);
            *vp = FETCH_OPND(-1);
            obj = NULL;
          END_CASE(JSOP_SETARG)

          BEGIN_CASE(JSOP_GETVAR)
            slot = GET_VARNO(pc);
            JS_ASSERT(slot < fp->fun->u.i.nvars);
            PUSH_OPND(fp->vars[slot]);
            obj = NULL;
          END_CASE(JSOP_GETVAR)

          BEGIN_CASE(JSOP_SETVAR)
            slot = GET_VARNO(pc);
            JS_ASSERT(slot < fp->fun->u.i.nvars);
            vp = &fp->vars[slot];
            GC_POKE(cx, *vp);
            *vp = FETCH_OPND(-1);
            obj = NULL;
          END_CASE(JSOP_SETVAR)

          BEGIN_CASE(JSOP_GETGVAR)
            slot = GET_VARNO(pc);
            JS_ASSERT(slot < fp->nvars);
            lval = fp->vars[slot];
            if (JSVAL_IS_NULL(lval)) {
                op = JSOP_NAME;
                DO_OP();
            }
            slot = JSVAL_TO_INT(lval);
            obj = fp->varobj;
            rval = OBJ_GET_SLOT(cx, obj, slot);
            PUSH_OPND(rval);
          END_CASE(JSOP_GETGVAR)

          BEGIN_CASE(JSOP_SETGVAR)
            slot = GET_VARNO(pc);
            JS_ASSERT(slot < fp->nvars);
            rval = FETCH_OPND(-1);
            lval = fp->vars[slot];
            obj = fp->varobj;
            if (JSVAL_IS_NULL(lval)) {
                /*
                 * Inline-clone and specialize JSOP_SETNAME code here because
                 * JSOP_SETGVAR has arity 1: [rval], not arity 2: [obj, rval]
                 * as JSOP_SETNAME does, where [obj] is due to JSOP_BINDNAME.
                 */
                atom = GET_ATOM(cx, script, pc);
                id = ATOM_TO_JSID(atom);
                SAVE_SP_AND_PC(fp);
                CACHED_SET(OBJ_SET_PROPERTY(cx, obj, id, &rval));
                if (!ok)
                    goto out;
                STORE_OPND(-1, rval);
            } else {
                slot = JSVAL_TO_INT(lval);
                GC_POKE(cx, obj->slots[slot]);
                OBJ_SET_SLOT(cx, obj, slot, rval);
            }
            obj = NULL;
          END_CASE(JSOP_SETGVAR)

          BEGIN_CASE(JSOP_DEFCONST)
          BEGIN_CASE(JSOP_DEFVAR)
            atomIndex = GET_ATOM_INDEX(pc);

          do_JSOP_DEFCONST:
          do_JSOP_DEFVAR:
            atom = js_GetAtom(cx, &script->atomMap, atomIndex);
            obj = fp->varobj;
            attrs = JSPROP_ENUMERATE;
            if (!(fp->flags & JSFRAME_EVAL))
                attrs |= JSPROP_PERMANENT;
            if (op == JSOP_DEFCONST)
                attrs |= JSPROP_READONLY;

            /* Lookup id in order to check for redeclaration problems. */
            id = ATOM_TO_JSID(atom);
            SAVE_SP_AND_PC(fp);
            ok = js_CheckRedeclaration(cx, obj, id, attrs, &obj2, &prop);
            if (!ok)
                goto out;

            /* Bind a variable only if it's not yet defined. */
            if (!prop) {
                ok = OBJ_DEFINE_PROPERTY(cx, obj, id, JSVAL_VOID, NULL, NULL,
                                         attrs, &prop);
                if (!ok)
                    goto out;
                JS_ASSERT(prop);
                obj2 = obj;
            }

            /*
             * Try to optimize a property we either just created, or found
             * directly in the global object, that is permanent, has a slot,
             * and has stub getter and setter, into a "fast global" accessed
             * by the JSOP_*GVAR opcodes.
             */
            if (atomIndex < script->numGlobalVars &&
                (attrs & JSPROP_PERMANENT) &&
                obj2 == obj &&
                OBJ_IS_NATIVE(obj)) {
                sprop = (JSScopeProperty *) prop;
                if (SPROP_HAS_VALID_SLOT(sprop, OBJ_SCOPE(obj)) &&
                    SPROP_HAS_STUB_GETTER(sprop) &&
                    SPROP_HAS_STUB_SETTER(sprop)) {
                    /*
                     * Fast globals use fp->vars to map the global name's
                     * atomIndex to the permanent fp->varobj slot number,
                     * tagged as a jsval.  The atomIndex for the global's
                     * name literal is identical to its fp->vars index.
                     */
                    fp->vars[atomIndex] = INT_TO_JSVAL(sprop->slot);
                }
            }

            OBJ_DROP_PROPERTY(cx, obj2, prop);
          END_CASE(JSOP_DEFVAR)

          BEGIN_LITOPX_CASE(JSOP_DEFFUN, 0)
            obj = ATOM_TO_OBJECT(atom);
            fun = (JSFunction *) JS_GetPrivate(cx, obj);
            id = ATOM_TO_JSID(fun->atom);

            /*
             * We must be at top-level (either outermost block that forms a
             * function's body, or a global) scope, not inside an expression
             * (JSOP_{ANON,NAMED}FUNOBJ) or compound statement (JSOP_CLOSURE)
             * in the same compilation unit (ECMA Program).
             *
             * However, we could be in a Program being eval'd from inside a
             * with statement, so we need to distinguish scope chain head from
             * variables object.  Hence the obj2 vs. parent distinction below.
             * First we make sure the function object we're defining has the
             * right scope chain.  Then we define its name in fp->varobj.
             *
             * If static link is not current scope, clone fun's object to link
             * to the current scope via parent.  This clause exists to enable
             * sharing of compiled functions among multiple equivalent scopes,
             * splitting the cost of compilation evenly among the scopes and
             * amortizing it over a number of executions.  Examples include XUL
             * scripts and event handlers shared among Mozilla chrome windows,
             * and server-side JS user-defined functions shared among requests.
             *
             * NB: The Script object exposes compile and exec in the language,
             * such that this clause introduces an incompatible change from old
             * JS versions that supported Script.  Such a JS version supported
             * executing a script that defined and called functions scoped by
             * the compile-time static link, not by the exec-time scope chain.
             *
             * We sacrifice compatibility, breaking such scripts, in order to
             * promote compile-cost sharing and amortizing, and because Script
             * is not and will not be standardized.
             */
            JS_ASSERT(!fp->blockChain);
            obj2 = fp->scopeChain;
            if (OBJ_GET_PARENT(cx, obj) != obj2) {
                obj = js_CloneFunctionObject(cx, obj, obj2);
                if (!obj) {
                    ok = JS_FALSE;
                    goto out;
                }
            }

            /*
             * Protect obj from any GC hiding below OBJ_DEFINE_PROPERTY.  All
             * paths from here must flow through the "Restore fp->scopeChain"
             * code below the OBJ_DEFINE_PROPERTY call.
             */
            fp->scopeChain = obj;
            rval = OBJECT_TO_JSVAL(obj);

            /*
             * ECMA requires functions defined when entering Global code to be
             * permanent, and functions defined when entering Eval code to be
             * impermanent.
             */
            attrs = JSPROP_ENUMERATE;
            if (!(fp->flags & JSFRAME_EVAL))
                attrs |= JSPROP_PERMANENT;

            /*
             * Load function flags that are also property attributes.  Getters
             * and setters do not need a slot, their value is stored elsewhere
             * in the property itself, not in obj->slots.
             */
            flags = JSFUN_GSFLAG2ATTR(fun->flags);
            if (flags) {
                attrs |= flags | JSPROP_SHARED;
                rval = JSVAL_VOID;
            }

            /*
             * Check for a const property of the same name -- or any kind
             * of property if executing with the strict option.  We check
             * here at runtime as well as at compile-time, to handle eval
             * as well as multiple HTML script tags.
             */
            parent = fp->varobj;
            SAVE_SP_AND_PC(fp);
            ok = js_CheckRedeclaration(cx, parent, id, attrs, NULL, NULL);
            if (ok) {
                ok = OBJ_DEFINE_PROPERTY(cx, parent, id, rval,
                                         (flags & JSPROP_GETTER)
                                         ? JS_EXTENSION (JSPropertyOp) obj
                                         : NULL,
                                         (flags & JSPROP_SETTER)
                                         ? JS_EXTENSION (JSPropertyOp) obj
                                         : NULL,
                                         attrs,
                                         &prop);
            }

            /* Restore fp->scopeChain now that obj is defined in fp->varobj. */
            fp->scopeChain = obj2;
            if (!ok)
                goto out;

#if 0
            if (attrs == (JSPROP_ENUMERATE | JSPROP_PERMANENT) &&
                script->numGlobalVars) {
                /*
                 * As with JSOP_DEFVAR and JSOP_DEFCONST (above), fast globals
                 * use fp->vars to map the global function name's atomIndex to
                 * its permanent fp->varobj slot number, tagged as a jsval.
                 */
                sprop = (JSScopeProperty *) prop;
                fp->vars[atomIndex] = INT_TO_JSVAL(sprop->slot);
            }
#endif
            OBJ_DROP_PROPERTY(cx, parent, prop);
          END_LITOPX_CASE(JSOP_DEFFUN)

          BEGIN_LITOPX_CASE(JSOP_DEFLOCALFUN, VARNO_LEN)
            /*
             * Define a local function (i.e., one nested at the top level of
             * another function), parented by the current scope chain, and
             * stored in a local variable slot that the compiler allocated.
             * This is an optimization over JSOP_DEFFUN that avoids requiring
             * a call object for the outer function's activation.
             */
            slot = GET_VARNO(pc2);
            obj = ATOM_TO_OBJECT(atom);

            JS_ASSERT(!fp->blockChain);
            if (!(fp->flags & JSFRAME_POP_BLOCKS)) {
                /*
                 * If the compiler-created function object (obj) is scoped by a
                 * let-induced body block, temporarily update fp->blockChain so
                 * that js_GetScopeChain will clone the block into the runtime
                 * scope needed to parent the function object's clone.
                 */
                parent = OBJ_GET_PARENT(cx, obj);
                if (OBJ_GET_CLASS(cx, parent) == &js_BlockClass)
                    fp->blockChain = parent;
                parent = js_GetScopeChain(cx, fp);
            } else {
                /*
                 * We have already emulated JSOP_ENTERBLOCK for the enclosing
                 * body block, for a prior JSOP_DEFLOCALFUN in the prolog,  so
                 * we just load fp->scopeChain into parent.
                 *
                 * In typical execution scenarios, the prolog bytecodes that
                 * include this JSOP_DEFLOCALFUN run, then come main bytecodes
                 * including JSOP_ENTERBLOCK for the outermost (body) block.
                 * JSOP_ENTERBLOCK will detect that it need not do anything if
                 * the body block was entered above due to a local function.
                 * Finally the matching JSOP_LEAVEBLOCK runs.
                 *
                 * If the matching JSOP_LEAVEBLOCK for the body block does not
                 * run for some reason, the body block will be properly "put"
                 * (via js_PutBlockObject) by the PutBlockObjects call at the
                 * bottom of js_Interpret.
                 */
                parent = fp->scopeChain;
                JS_ASSERT(OBJ_GET_CLASS(cx, parent) == &js_BlockClass);
                JS_ASSERT(OBJ_GET_PROTO(cx, parent) == OBJ_GET_PARENT(cx, obj));
                JS_ASSERT(OBJ_GET_CLASS(cx, OBJ_GET_PARENT(cx, parent))
                          == &js_CallClass);
            }

            /* If re-parenting, store a clone of the function object. */
            if (OBJ_GET_PARENT(cx, obj) != parent) {
                SAVE_SP_AND_PC(fp);
                obj = js_CloneFunctionObject(cx, obj, parent);
                if (!obj) {
                    ok = JS_FALSE;
                    goto out;
                }
            }
            fp->vars[slot] = OBJECT_TO_JSVAL(obj);
          END_LITOPX_CASE(JSOP_DEFLOCALFUN)

          BEGIN_LITOPX_CASE(JSOP_ANONFUNOBJ, 0)
            /* Push the specified function object literal. */
            obj = ATOM_TO_OBJECT(atom);

            /* If re-parenting, push a clone of the function object. */
            SAVE_SP_AND_PC(fp);
            parent = js_GetScopeChain(cx, fp);
            if (!parent) {
                ok = JS_FALSE;
                goto out;
            }
            if (OBJ_GET_PARENT(cx, obj) != parent) {
                obj = js_CloneFunctionObject(cx, obj, parent);
                if (!obj) {
                    ok = JS_FALSE;
                    goto out;
                }
            }
            PUSH_OPND(OBJECT_TO_JSVAL(obj));
            obj = NULL;
          END_LITOPX_CASE(JSOP_ANONFUNOBJ)

          BEGIN_LITOPX_CASE(JSOP_NAMEDFUNOBJ, 0)
            /* ECMA ed. 3 FunctionExpression: function Identifier [etc.]. */
            rval = ATOM_KEY(atom);
            JS_ASSERT(VALUE_IS_FUNCTION(cx, rval));

            /*
             * 1. Create a new object as if by the expression new Object().
             * 2. Add Result(1) to the front of the scope chain.
             *
             * Step 2 is achieved by making the new object's parent be the
             * current scope chain, and then making the new object the parent
             * of the Function object clone.
             */
            SAVE_SP_AND_PC(fp);
            obj2 = js_GetScopeChain(cx, fp);
            if (!obj2) {
                ok = JS_FALSE;
                goto out;
            }
            parent = js_NewObject(cx, &js_ObjectClass, NULL, obj2);
            if (!parent) {
                ok = JS_FALSE;
                goto out;
            }

            /*
             * 3. Create a new Function object as specified in section 13.2
             * with [parameters and body specified by the function expression
             * that was parsed by the compiler into a Function object, and
             * saved in the script's atom map].
             *
             * Protect parent from GC after js_CloneFunctionObject calls into
             * js_NewObject, which displaces the newborn object root in cx by
             * allocating the clone, then runs a last-ditch GC while trying
             * to allocate the clone's slots vector.  Another, multi-threaded
             * path: js_CloneFunctionObject => js_NewObject => OBJ_GET_CLASS
             * which may suspend the current request in ClaimScope, with the
             * newborn displaced as in the first scenario.
             */
            fp->scopeChain = parent;
            obj = js_CloneFunctionObject(cx, JSVAL_TO_OBJECT(rval), parent);
            if (!obj) {
                ok = JS_FALSE;
                goto out;
            }

            /*
             * Protect obj from any GC hiding below OBJ_DEFINE_PROPERTY.  All
             * paths from here must flow through the "Restore fp->scopeChain"
             * code below the OBJ_DEFINE_PROPERTY call.
             */
            fp->scopeChain = obj;
            rval = OBJECT_TO_JSVAL(obj);

            /*
             * 4. Create a property in the object Result(1).  The property's
             * name is [fun->atom, the identifier parsed by the compiler],
             * value is Result(3), and attributes are { DontDelete, ReadOnly }.
             */
            fun = (JSFunction *) JS_GetPrivate(cx, obj);
            attrs = JSFUN_GSFLAG2ATTR(fun->flags);
            if (attrs) {
                attrs |= JSPROP_SHARED;
                rval = JSVAL_VOID;
            }
            ok = OBJ_DEFINE_PROPERTY(cx, parent, ATOM_TO_JSID(fun->atom), rval,
                                     (attrs & JSPROP_GETTER)
                                     ? JS_EXTENSION (JSPropertyOp) obj
                                     : NULL,
                                     (attrs & JSPROP_SETTER)
                                     ? JS_EXTENSION (JSPropertyOp) obj
                                     : NULL,
                                     attrs |
                                     JSPROP_ENUMERATE | JSPROP_PERMANENT |
                                     JSPROP_READONLY,
                                     NULL);

            /* Restore fp->scopeChain now that obj is defined in parent. */
            fp->scopeChain = obj2;
            if (!ok) {
                cx->weakRoots.newborn[GCX_OBJECT] = NULL;
                goto out;
            }

            /*
             * 5. Remove Result(1) from the front of the scope chain [no-op].
             * 6. Return Result(3).
             */
            PUSH_OPND(OBJECT_TO_JSVAL(obj));
            obj = NULL;
          END_LITOPX_CASE(JSOP_NAMEDFUNOBJ)

          BEGIN_LITOPX_CASE(JSOP_CLOSURE, 0)
            /*
             * ECMA ed. 3 extension: a named function expression in a compound
             * statement (not at the top statement level of global code, or at
             * the top level of a function body).
             *
             * Get immediate operand atom, which is a function object literal.
             * From it, get the function to close.
             */
            JS_ASSERT(VALUE_IS_FUNCTION(cx, ATOM_KEY(atom)));
            obj = ATOM_TO_OBJECT(atom);

            /*
             * Clone the function object with the current scope chain as the
             * clone's parent.  The original function object is the prototype
             * of the clone.  Do this only if re-parenting; the compiler may
             * have seen the right parent already and created a sufficiently
             * well-scoped function object.
             */
            SAVE_SP_AND_PC(fp);
            obj2 = js_GetScopeChain(cx, fp);
            if (!obj2) {
                ok = JS_FALSE;
                goto out;
            }
            if (OBJ_GET_PARENT(cx, obj) != obj2) {
                obj = js_CloneFunctionObject(cx, obj, obj2);
                if (!obj) {
                    ok = JS_FALSE;
                    goto out;
                }
            }

            /*
             * Protect obj from any GC hiding below OBJ_DEFINE_PROPERTY.  All
             * paths from here must flow through the "Restore fp->scopeChain"
             * code below the OBJ_DEFINE_PROPERTY call.
             */
            fp->scopeChain = obj;
            rval = OBJECT_TO_JSVAL(obj);

            /*
             * Make a property in fp->varobj with id fun->atom and value obj,
             * unless fun is a getter or setter (in which case, obj is cast to
             * a JSPropertyOp and passed accordingly).
             */
            fun = (JSFunction *) JS_GetPrivate(cx, obj);
            attrs = JSFUN_GSFLAG2ATTR(fun->flags);
            if (attrs) {
                attrs |= JSPROP_SHARED;
                rval = JSVAL_VOID;
            }
            parent = fp->varobj;
            ok = OBJ_DEFINE_PROPERTY(cx, parent, ATOM_TO_JSID(fun->atom), rval,
                                     (attrs & JSPROP_GETTER)
                                     ? JS_EXTENSION (JSPropertyOp) obj
                                     : NULL,
                                     (attrs & JSPROP_SETTER)
                                     ? JS_EXTENSION (JSPropertyOp) obj
                                     : NULL,
                                     attrs | JSPROP_ENUMERATE
                                           | JSPROP_PERMANENT,
                                     &prop);

            /* Restore fp->scopeChain now that obj is defined in fp->varobj. */
            fp->scopeChain = obj2;
            if (!ok) {
                cx->weakRoots.newborn[GCX_OBJECT] = NULL;
                goto out;
            }

#if 0
            if (attrs == 0 && script->numGlobalVars) {
                /*
                 * As with JSOP_DEFVAR and JSOP_DEFCONST (above), fast globals
                 * use fp->vars to map the global function name's atomIndex to
                 * its permanent fp->varobj slot number, tagged as a jsval.
                 */
                sprop = (JSScopeProperty *) prop;
                fp->vars[atomIndex] = INT_TO_JSVAL(sprop->slot);
            }
#endif
            OBJ_DROP_PROPERTY(cx, parent, prop);
          END_LITOPX_CASE(JSOP_CLOSURE)

#if JS_HAS_GETTER_SETTER
          BEGIN_CASE(JSOP_GETTER)
          BEGIN_CASE(JSOP_SETTER)
            op2 = (JSOp) *++pc;
            switch (op2) {
              case JSOP_SETNAME:
              case JSOP_SETPROP:
                atom = GET_ATOM(cx, script, pc);
                id   = ATOM_TO_JSID(atom);
                rval = FETCH_OPND(-1);
                i = -1;
                goto gs_pop_lval;

              case JSOP_SETELEM:
                rval = FETCH_OPND(-1);
                FETCH_ELEMENT_ID(-2, id);
                i = -2;
              gs_pop_lval:
                FETCH_OBJECT(cx, i - 1, lval, obj);
                break;

              case JSOP_INITPROP:
                JS_ASSERT(sp - fp->spbase >= 2);
                rval = FETCH_OPND(-1);
                i = -1;
                atom = GET_ATOM(cx, script, pc);
                id   = ATOM_TO_JSID(atom);
                goto gs_get_lval;

              case JSOP_INITELEM:
                JS_ASSERT(sp - fp->spbase >= 3);
                rval = FETCH_OPND(-1);
                FETCH_ELEMENT_ID(-2, id);
                i = -2;
              gs_get_lval:
                lval = FETCH_OPND(i-1);
                JS_ASSERT(JSVAL_IS_OBJECT(lval));
                obj = JSVAL_TO_OBJECT(lval);
                break;

              default:
                JS_ASSERT(0);
            }

            /* Ensure that id has a type suitable for use with obj. */
            CHECK_ELEMENT_ID(obj, id);

            SAVE_SP_AND_PC(fp);
            if (JS_TypeOfValue(cx, rval) != JSTYPE_FUNCTION) {
                JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL,
                                     JSMSG_BAD_GETTER_OR_SETTER,
                                     (op == JSOP_GETTER)
                                     ? js_getter_str
                                     : js_setter_str);
                ok = JS_FALSE;
                goto out;
            }

            /*
             * Getters and setters are just like watchpoints from an access
             * control point of view.
             */
            ok = OBJ_CHECK_ACCESS(cx, obj, id, JSACC_WATCH, &rtmp, &attrs);
            if (!ok)
                goto out;

            if (op == JSOP_GETTER) {
                getter = JS_EXTENSION (JSPropertyOp) JSVAL_TO_OBJECT(rval);
                setter = NULL;
                attrs = JSPROP_GETTER;
            } else {
                getter = NULL;
                setter = JS_EXTENSION (JSPropertyOp) JSVAL_TO_OBJECT(rval);
                attrs = JSPROP_SETTER;
            }
            attrs |= JSPROP_ENUMERATE | JSPROP_SHARED;

            /* Check for a readonly or permanent property of the same name. */
            ok = js_CheckRedeclaration(cx, obj, id, attrs, NULL, NULL);
            if (!ok)
                goto out;

            ok = OBJ_DEFINE_PROPERTY(cx, obj, id, JSVAL_VOID, getter, setter,
                                     attrs, NULL);
            if (!ok)
                goto out;

            obj = NULL;
            sp += i;
            if (js_CodeSpec[op2].ndefs)
                STORE_OPND(-1, rval);
            len = js_CodeSpec[op2].length;
            DO_NEXT_OP(len);
#endif /* JS_HAS_GETTER_SETTER */

          BEGIN_CASE(JSOP_NEWINIT)
            argc = 0;
            fp->sharpDepth++;
            goto do_new;

          BEGIN_CASE(JSOP_ENDINIT)
            if (--fp->sharpDepth == 0)
                fp->sharpArray = NULL;

            /* Re-set the newborn root to the top of this object tree. */
            JS_ASSERT(sp - fp->spbase >= 1);
            lval = FETCH_OPND(-1);
            JS_ASSERT(JSVAL_IS_OBJECT(lval));
            cx->weakRoots.newborn[GCX_OBJECT] = JSVAL_TO_GCTHING(lval);
          END_CASE(JSOP_ENDINIT)

          BEGIN_CASE(JSOP_INITPROP)
            /* Pop the property's value into rval. */
            JS_ASSERT(sp - fp->spbase >= 2);
            rval = FETCH_OPND(-1);

            /* Get the immediate property name into id. */
            atom = GET_ATOM(cx, script, pc);
            id   = ATOM_TO_JSID(atom);
            i = -1;
            goto do_init;

          BEGIN_CASE(JSOP_INITELEM)
            /* Pop the element's value into rval. */
            JS_ASSERT(sp - fp->spbase >= 3);
            rval = FETCH_OPND(-1);

            /* Pop and conditionally atomize the element id. */
            FETCH_ELEMENT_ID(-2, id);
            i = -2;

          do_init:
            /* Find the object being initialized at top of stack. */
            lval = FETCH_OPND(i-1);
            JS_ASSERT(JSVAL_IS_OBJECT(lval));
            obj = JSVAL_TO_OBJECT(lval);

            /* Ensure that id has a type suitable for use with obj. */
            CHECK_ELEMENT_ID(obj, id);

            /* Set the property named by obj[id] to rval. */
            SAVE_SP_AND_PC(fp);
            ok = OBJ_SET_PROPERTY(cx, obj, id, &rval);
            if (!ok)
                goto out;
            sp += i;
            len = js_CodeSpec[op].length;
            DO_NEXT_OP(len);

#if JS_HAS_SHARP_VARS
          BEGIN_CASE(JSOP_DEFSHARP)
            SAVE_SP_AND_PC(fp);
            obj = fp->sharpArray;
            if (!obj) {
                obj = js_NewArrayObject(cx, 0, NULL);
                if (!obj) {
                    ok = JS_FALSE;
                    goto out;
                }
                fp->sharpArray = obj;
            }
            i = (jsint) GET_ATOM_INDEX(pc);
            id = INT_TO_JSID(i);
            rval = FETCH_OPND(-1);
            if (JSVAL_IS_PRIMITIVE(rval)) {
                char numBuf[12];
                JS_snprintf(numBuf, sizeof numBuf, "%u", (unsigned) i);
                JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL,
                                     JSMSG_BAD_SHARP_DEF, numBuf);
                ok = JS_FALSE;
                goto out;
            }
            ok = OBJ_SET_PROPERTY(cx, obj, id, &rval);
            if (!ok)
                goto out;
          END_CASE(JSOP_DEFSHARP)

          BEGIN_CASE(JSOP_USESHARP)
            i = (jsint) GET_ATOM_INDEX(pc);
            id = INT_TO_JSID(i);
            obj = fp->sharpArray;
            if (!obj) {
                rval = JSVAL_VOID;
            } else {
                SAVE_SP_AND_PC(fp);
                ok = OBJ_GET_PROPERTY(cx, obj, id, &rval);
                if (!ok)
                    goto out;
            }
            if (!JSVAL_IS_OBJECT(rval)) {
                char numBuf[12];
                JS_snprintf(numBuf, sizeof numBuf, "%u", (unsigned) i);

                SAVE_SP_AND_PC(fp);
                JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL,
                                     JSMSG_BAD_SHARP_USE, numBuf);
                ok = JS_FALSE;
                goto out;
            }
            PUSH_OPND(rval);
          END_CASE(JSOP_USESHARP)
#endif /* JS_HAS_SHARP_VARS */

          /* No-ops for ease of decompilation and jit'ing. */
          EMPTY_CASE(JSOP_TRY)
          EMPTY_CASE(JSOP_FINALLY)

          /* Reset the stack to the given depth. */
          BEGIN_CASE(JSOP_SETSP)
            i = (jsint) GET_ATOM_INDEX(pc);
            JS_ASSERT(i >= 0);

            for (obj = fp->blockChain; obj; obj = OBJ_GET_PARENT(cx, obj)) {
                JS_ASSERT(OBJ_GET_CLASS(cx, obj) == &js_BlockClass);
                if (OBJ_BLOCK_DEPTH(cx, obj) + (jsint)OBJ_BLOCK_COUNT(cx, obj) <= i) {
                    JS_ASSERT(OBJ_BLOCK_DEPTH(cx, obj) < i || OBJ_BLOCK_COUNT(cx, obj) == 0);
                    break;
                }
            }
            fp->blockChain = obj;

            JS_ASSERT(ok);
            for (obj = fp->scopeChain;
                 (clasp = OBJ_GET_CLASS(cx, obj)) == &js_WithClass ||
                 clasp == &js_BlockClass;
                 obj = OBJ_GET_PARENT(cx, obj)) {
                if (JS_GetPrivate(cx, obj) != fp ||
                    OBJ_BLOCK_DEPTH(cx, obj) < i) {
                    break;
                }
                if (clasp == &js_BlockClass)
                    ok &= js_PutBlockObject(cx, obj);
                else
                    JS_SetPrivate(cx, obj, NULL);
            }

            fp->scopeChain = obj;

            /* Set sp after js_PutBlockObject to avoid potential GC hazards. */
            sp = fp->spbase + i;

            /* Don't fail until after we've updated all stacks. */
            if (!ok)
                goto out;
          END_CASE(JSOP_SETSP)

          BEGIN_CASE(JSOP_GOSUB)
            JS_ASSERT(cx->exception != JSVAL_HOLE);
            if (!cx->throwing) {
                lval = JSVAL_HOLE;
            } else {
                lval = cx->exception;
                cx->throwing = JS_FALSE;
            }
            PUSH(lval);
            i = PTRDIFF(pc, script->main, jsbytecode) + JSOP_GOSUB_LENGTH;
            len = GET_JUMP_OFFSET(pc);
            PUSH(INT_TO_JSVAL(i));
          END_VARLEN_CASE

          BEGIN_CASE(JSOP_GOSUBX)
            JS_ASSERT(cx->exception != JSVAL_HOLE);
            if (!cx->throwing) {
                lval = JSVAL_HOLE;
            } else {
                lval = cx->exception;
                cx->throwing = JS_FALSE;
            }
            PUSH(lval);
            i = PTRDIFF(pc, script->main, jsbytecode) + JSOP_GOSUBX_LENGTH;
            len = GET_JUMPX_OFFSET(pc);
            PUSH(INT_TO_JSVAL(i));
          END_VARLEN_CASE

          BEGIN_CASE(JSOP_RETSUB)
            rval = POP();
            JS_ASSERT(JSVAL_IS_INT(rval));
            lval = POP();
            if (lval != JSVAL_HOLE) {
                /*
                 * Exception was pending during finally, throw it *before* we
                 * adjust pc, because pc indexes into script->trynotes.  This
                 * turns out not to be necessary, but it seems clearer.  And
                 * it points out a FIXME: 350509, due to Igor Bukanov.
                 */
                cx->throwing = JS_TRUE;
                cx->exception = lval;
                ok = JS_FALSE;
                goto out;
            }
            len = JSVAL_TO_INT(rval);
            pc = script->main;
          END_VARLEN_CASE

          BEGIN_CASE(JSOP_EXCEPTION)
            JS_ASSERT(cx->throwing);
            PUSH(cx->exception);
            cx->throwing = JS_FALSE;
          END_CASE(JSOP_EXCEPTION)

          BEGIN_CASE(JSOP_THROWING)
            JS_ASSERT(!cx->throwing);
            cx->throwing = JS_TRUE;
            cx->exception = POP_OPND();
          END_CASE(JSOP_THROWING)

          BEGIN_CASE(JSOP_THROW)
            JS_ASSERT(!cx->throwing);
            cx->throwing = JS_TRUE;
            cx->exception = POP_OPND();
            ok = JS_FALSE;
            /* let the code at out try to catch the exception. */
            goto out;

          BEGIN_CASE(JSOP_SETLOCALPOP)
            /*
             * The stack must have a block with at least one local slot below
             * the exception object.
             */
            JS_ASSERT(sp - fp->spbase >= 2);
            slot = GET_UINT16(pc);
            JS_ASSERT(slot + 1 < (uintN)depth);
            fp->spbase[slot] = POP_OPND();
          END_CASE(JSOP_SETLOCALPOP)

          BEGIN_CASE(JSOP_INSTANCEOF)
            SAVE_SP_AND_PC(fp);
            rval = FETCH_OPND(-1);
            if (JSVAL_IS_PRIMITIVE(rval) ||
                !(obj = JSVAL_TO_OBJECT(rval))->map->ops->hasInstance) {
                str = js_DecompileValueGenerator(cx, -1, rval, NULL);
                if (str) {
                    JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL,
                                         JSMSG_BAD_INSTANCEOF_RHS,
                                         JS_GetStringBytes(str));
                }
                ok = JS_FALSE;
                goto out;
            }
            lval = FETCH_OPND(-2);
            cond = JS_FALSE;
            ok = obj->map->ops->hasInstance(cx, obj, lval, &cond);
            if (!ok)
                goto out;
            sp--;
            STORE_OPND(-1, BOOLEAN_TO_JSVAL(cond));
          END_CASE(JSOP_INSTANCEOF)

#if JS_HAS_DEBUGGER_KEYWORD
          BEGIN_CASE(JSOP_DEBUGGER)
          {
            JSTrapHandler handler = rt->debuggerHandler;
            if (handler) {
                SAVE_SP_AND_PC(fp);
                switch (handler(cx, script, pc, &rval,
                                rt->debuggerHandlerData)) {
                  case JSTRAP_ERROR:
                    ok = JS_FALSE;
                    goto out;
                  case JSTRAP_CONTINUE:
                    break;
                  case JSTRAP_RETURN:
                    fp->rval = rval;
                    goto out;
                  case JSTRAP_THROW:
                    cx->throwing = JS_TRUE;
                    cx->exception = rval;
                    ok = JS_FALSE;
                    goto out;
                  default:;
                }
                LOAD_INTERRUPT_HANDLER(rt);
            }
          }
          END_CASE(JSOP_DEBUGGER)
#endif /* JS_HAS_DEBUGGER_KEYWORD */

#if JS_HAS_XML_SUPPORT
          BEGIN_CASE(JSOP_DEFXMLNS)
            rval = POP();
            SAVE_SP_AND_PC(fp);
            ok = js_SetDefaultXMLNamespace(cx, rval);
            if (!ok)
                goto out;
          END_CASE(JSOP_DEFXMLNS)

          BEGIN_CASE(JSOP_ANYNAME)
            SAVE_SP_AND_PC(fp);
            ok = js_GetAnyName(cx, &rval);
            if (!ok)
                goto out;
            PUSH_OPND(rval);
          END_CASE(JSOP_ANYNAME)

          BEGIN_LITOPX_CASE(JSOP_QNAMEPART, 0)
            PUSH_OPND(ATOM_KEY(atom));
          END_LITOPX_CASE(JSOP_QNAMEPART)

          BEGIN_LITOPX_CASE(JSOP_QNAMECONST, 0)
            rval = ATOM_KEY(atom);
            lval = FETCH_OPND(-1);
            SAVE_SP_AND_PC(fp);
            obj = js_ConstructXMLQNameObject(cx, lval, rval);
            if (!obj) {
                ok = JS_FALSE;
                goto out;
            }
            STORE_OPND(-1, OBJECT_TO_JSVAL(obj));
          END_LITOPX_CASE(JSOP_QNAMECONST)

          BEGIN_CASE(JSOP_QNAME)
            rval = FETCH_OPND(-1);
            lval = FETCH_OPND(-2);
            SAVE_SP_AND_PC(fp);
            obj = js_ConstructXMLQNameObject(cx, lval, rval);
            if (!obj) {
                ok = JS_FALSE;
                goto out;
            }
            sp--;
            STORE_OPND(-1, OBJECT_TO_JSVAL(obj));
          END_CASE(JSOP_QNAME)

          BEGIN_CASE(JSOP_TOATTRNAME)
            rval = FETCH_OPND(-1);
            SAVE_SP_AND_PC(fp);
            ok = js_ToAttributeName(cx, &rval);
            if (!ok)
                goto out;
            STORE_OPND(-1, rval);
          END_CASE(JSOP_TOATTRNAME)

          BEGIN_CASE(JSOP_TOATTRVAL)
            rval = FETCH_OPND(-1);
            JS_ASSERT(JSVAL_IS_STRING(rval));
            SAVE_SP_AND_PC(fp);
            str = js_EscapeAttributeValue(cx, JSVAL_TO_STRING(rval));
            if (!str) {
                ok = JS_FALSE;
                goto out;
            }
            STORE_OPND(-1, STRING_TO_JSVAL(str));
          END_CASE(JSOP_TOATTRVAL)

          BEGIN_CASE(JSOP_ADDATTRNAME)
          BEGIN_CASE(JSOP_ADDATTRVAL)
            rval = FETCH_OPND(-1);
            lval = FETCH_OPND(-2);
            str = JSVAL_TO_STRING(lval);
            str2 = JSVAL_TO_STRING(rval);
            SAVE_SP_AND_PC(fp);
            str = js_AddAttributePart(cx, op == JSOP_ADDATTRNAME, str, str2);
            if (!str) {
                ok = JS_FALSE;
                goto out;
            }
            sp--;
            STORE_OPND(-1, STRING_TO_JSVAL(str));
          END_CASE(JSOP_ADDATTRNAME)

          BEGIN_CASE(JSOP_BINDXMLNAME)
            lval = FETCH_OPND(-1);
            SAVE_SP_AND_PC(fp);
            ok = js_FindXMLProperty(cx, lval, &obj, &rval);
            if (!ok)
                goto out;
            STORE_OPND(-1, OBJECT_TO_JSVAL(obj));
            PUSH_OPND(rval);
          END_CASE(JSOP_BINDXMLNAME)

          BEGIN_CASE(JSOP_SETXMLNAME)
            obj = JSVAL_TO_OBJECT(FETCH_OPND(-3));
            lval = FETCH_OPND(-2);
            rval = FETCH_OPND(-1);
            SAVE_SP_AND_PC(fp);
            ok = js_SetXMLProperty(cx, obj, lval, &rval);
            if (!ok)
                goto out;
            sp -= 2;
            STORE_OPND(-1, rval);
            obj = NULL;
          END_CASE(JSOP_SETXMLNAME)

          BEGIN_CASE(JSOP_XMLNAME)
            lval = FETCH_OPND(-1);
            SAVE_SP_AND_PC(fp);
            ok = js_FindXMLProperty(cx, lval, &obj, &rval);
            if (!ok)
                goto out;
            ok = js_GetXMLProperty(cx, obj, rval, &rval);
            if (!ok)
                goto out;
            STORE_OPND(-1, rval);
          END_CASE(JSOP_XMLNAME)

          BEGIN_CASE(JSOP_DESCENDANTS)
          BEGIN_CASE(JSOP_DELDESC)
            FETCH_OBJECT(cx, -2, lval, obj);
            rval = FETCH_OPND(-1);
            SAVE_SP_AND_PC(fp);
            ok = js_GetXMLDescendants(cx, obj, rval, &rval);
            if (!ok)
                goto out;

            if (op == JSOP_DELDESC) {
                sp[-1] = rval;          /* set local root */
                ok = js_DeleteXMLListElements(cx, JSVAL_TO_OBJECT(rval));
                if (!ok)
                    goto out;
                rval = JSVAL_TRUE;      /* always succeed */
            }

            sp--;
            STORE_OPND(-1, rval);
          END_CASE(JSOP_DESCENDANTS)

          BEGIN_CASE(JSOP_FILTER)
            FETCH_OBJECT(cx, -1, lval, obj);
            len = GET_JUMP_OFFSET(pc);
            SAVE_SP_AND_PC(fp);
            ok = js_FilterXMLList(cx, obj, pc + js_CodeSpec[op].length, &rval);
            if (!ok)
                goto out;
            JS_ASSERT(fp->sp == sp);
            STORE_OPND(-1, rval);
          END_VARLEN_CASE

          BEGIN_CASE(JSOP_ENDFILTER)
            *result = POP_OPND();
            goto out;

          EMPTY_CASE(JSOP_STARTXML)
          EMPTY_CASE(JSOP_STARTXMLEXPR)

          BEGIN_CASE(JSOP_TOXML)
            rval = FETCH_OPND(-1);
            SAVE_SP_AND_PC(fp);
            obj = js_ValueToXMLObject(cx, rval);
            if (!obj) {
                ok = JS_FALSE;
                goto out;
            }
            STORE_OPND(-1, OBJECT_TO_JSVAL(obj));
          END_CASE(JSOP_TOXML)

          BEGIN_CASE(JSOP_TOXMLLIST)
            rval = FETCH_OPND(-1);
            SAVE_SP_AND_PC(fp);
            obj = js_ValueToXMLListObject(cx, rval);
            if (!obj) {
                ok = JS_FALSE;
                goto out;
            }
            STORE_OPND(-1, OBJECT_TO_JSVAL(obj));
          END_CASE(JSOP_TOXMLLIST)

          BEGIN_CASE(JSOP_XMLTAGEXPR)
            rval = FETCH_OPND(-1);
            SAVE_SP_AND_PC(fp);
            str = js_ValueToString(cx, rval);
            if (!str) {
                ok = JS_FALSE;
                goto out;
            }
            STORE_OPND(-1, STRING_TO_JSVAL(str));
          END_CASE(JSOP_XMLTAGEXPR)

          BEGIN_CASE(JSOP_XMLELTEXPR)
            rval = FETCH_OPND(-1);
            SAVE_SP_AND_PC(fp);
            if (VALUE_IS_XML(cx, rval)) {
                str = js_ValueToXMLString(cx, rval);
            } else {
                str = js_ValueToString(cx, rval);
                if (str)
                    str = js_EscapeElementValue(cx, str);
            }
            if (!str) {
                ok = JS_FALSE;
                goto out;
            }
            STORE_OPND(-1, STRING_TO_JSVAL(str));
          END_CASE(JSOP_XMLELTEXPR)

          BEGIN_LITOPX_CASE(JSOP_XMLOBJECT, 0)
            SAVE_SP_AND_PC(fp);
            obj = js_CloneXMLObject(cx, ATOM_TO_OBJECT(atom));
            if (!obj) {
                ok = JS_FALSE;
                goto out;
            }
            PUSH_OPND(OBJECT_TO_JSVAL(obj));
            obj = NULL;
          END_LITOPX_CASE(JSOP_XMLOBJECT)

          BEGIN_LITOPX_CASE(JSOP_XMLCDATA, 0)
            str = ATOM_TO_STRING(atom);
            obj = js_NewXMLSpecialObject(cx, JSXML_CLASS_TEXT, NULL, str);
            if (!obj) {
                ok = JS_FALSE;
                goto out;
            }
            PUSH_OPND(OBJECT_TO_JSVAL(obj));
          END_LITOPX_CASE(JSOP_XMLCDATA)

          BEGIN_LITOPX_CASE(JSOP_XMLCOMMENT, 0)
            str = ATOM_TO_STRING(atom);
            obj = js_NewXMLSpecialObject(cx, JSXML_CLASS_COMMENT, NULL, str);
            if (!obj) {
                ok = JS_FALSE;
                goto out;
            }
            PUSH_OPND(OBJECT_TO_JSVAL(obj));
          END_LITOPX_CASE(JSOP_XMLCOMMENT)

          BEGIN_LITOPX_CASE(JSOP_XMLPI, 0)
            str = ATOM_TO_STRING(atom);
            rval = FETCH_OPND(-1);
            str2 = JSVAL_TO_STRING(rval);
            SAVE_SP_AND_PC(fp);
            obj = js_NewXMLSpecialObject(cx,
                                         JSXML_CLASS_PROCESSING_INSTRUCTION,
                                         str, str2);
            if (!obj) {
                ok = JS_FALSE;
                goto out;
            }
            STORE_OPND(-1, OBJECT_TO_JSVAL(obj));
          END_LITOPX_CASE(JSOP_XMLPI)

          BEGIN_LITOPX_CASE(JSOP_GETMETHOD, 0)
            /* Get an immediate atom naming the property. */
            id   = ATOM_TO_JSID(atom);
            lval = FETCH_OPND(-1);
            SAVE_SP_AND_PC(fp);
            if (!JSVAL_IS_PRIMITIVE(lval)) {
                STORE_OPND(-1, lval);
                obj = JSVAL_TO_OBJECT(lval);

                /* Special-case XML object method lookup, per ECMA-357. */
                if (OBJECT_IS_XML(cx, obj)) {
                    JSXMLObjectOps *ops;

                    ops = (JSXMLObjectOps *) obj->map->ops;
                    obj = ops->getMethod(cx, obj, id, &rval);
                    if (!obj)
                        ok = JS_FALSE;
                } else {
                    CACHED_GET(OBJ_GET_PROPERTY(cx, obj, id, &rval));
                }
            } else {
                if (JSVAL_IS_STRING(lval)) {
                    i = JSProto_String;
                } else if (JSVAL_IS_NUMBER(lval)) {
                    i = JSProto_Number;
                } else if (JSVAL_IS_BOOLEAN(lval)) {
                    i = JSProto_Boolean;
                } else {
                    JS_ASSERT(JSVAL_IS_NULL(lval) || JSVAL_IS_VOID(lval));
                    str = js_DecompileValueGenerator(cx, JSDVG_SEARCH_STACK,
                                                     lval, NULL);
                    if (str) {
                        JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL,
                                             JSMSG_NO_PROPERTIES,
                                             JS_GetStringBytes(str));
                    }
                    ok = JS_FALSE;
                    goto out;
                }
                ok = js_GetClassPrototype(cx, NULL, INT_TO_JSID(i), &obj);
                if (!ok)
                    goto out;
                JS_ASSERT(obj);
                STORE_OPND(-1, OBJECT_TO_JSVAL(obj));
                CACHED_GET(OBJ_GET_PROPERTY(cx, obj, id, &rval));
                obj = (JSObject *) lval; /* keep tagged as non-object */
            }
            if (!ok)
                goto out;
            STORE_OPND(-1, rval);
          END_LITOPX_CASE(JSOP_GETMETHOD)

          BEGIN_LITOPX_CASE(JSOP_SETMETHOD, 0)
            /* Get an immediate atom naming the property. */
            id   = ATOM_TO_JSID(atom);
            rval = FETCH_OPND(-1);
            FETCH_OBJECT(cx, -2, lval, obj);
            SAVE_SP_AND_PC(fp);

            /* Special-case XML object method lookup, per ECMA-357. */
            if (OBJECT_IS_XML(cx, obj)) {
                JSXMLObjectOps *ops;

                ops = (JSXMLObjectOps *) obj->map->ops;
                ok = ops->setMethod(cx, obj, id, &rval);
            } else {
                CACHED_SET(OBJ_SET_PROPERTY(cx, obj, id, &rval));
            }
            if (!ok)
                goto out;
            --sp;
            STORE_OPND(-1, rval);
            obj = NULL;
          END_LITOPX_CASE(JSOP_SETMETHOD)

          BEGIN_CASE(JSOP_GETFUNNS)
            SAVE_SP_AND_PC(fp);
            ok = js_GetFunctionNamespace(cx, &rval);
            if (!ok)
                goto out;
            PUSH_OPND(rval);
          END_CASE(JSOP_GETFUNNS)
#endif /* JS_HAS_XML_SUPPORT */

          BEGIN_LITOPX_CASE(JSOP_ENTERBLOCK, 0)
            obj = ATOM_TO_OBJECT(atom);
            JS_ASSERT(fp->spbase + OBJ_BLOCK_DEPTH(cx, obj) == sp);
            vp = sp + OBJ_BLOCK_COUNT(cx, obj);
            JS_ASSERT(vp <= fp->spbase + depth);
            while (sp < vp) {
                STORE_OPND(0, JSVAL_VOID);
                sp++;
            }

            /*
             * If this frame had to reflect the compile-time block chain into
             * the runtime scope chain, we can't optimize block scopes out of
             * runtime any longer, because an outer block that parents obj has
             * been cloned onto the scope chain.  To avoid re-cloning such a
             * parent and accumulating redundant clones via js_GetScopeChain,
             * we must clone each block eagerly on entry, and push it on the
             * scope chain, until this frame pops.
             */
            if (fp->flags & JSFRAME_POP_BLOCKS) {
                JS_ASSERT(!fp->blockChain);

                /*
                 * Check whether JSOP_DEFLOCALFUN emulated JSOP_ENTERBLOCK for
                 * the body block in order to correctly scope the local cloned
                 * function object it creates.
                 */
                parent = fp->scopeChain;
                if (OBJ_GET_PROTO(cx, parent) == obj) {
                    JS_ASSERT(OBJ_GET_CLASS(cx, parent) == &js_BlockClass);
                } else {
                    obj = js_CloneBlockObject(cx, obj, parent, fp);
                    if (!obj) {
                        ok = JS_FALSE;
                        goto out;
                    }
                    fp->scopeChain = obj;
                }
            } else {
                JS_ASSERT(!fp->blockChain ||
                          OBJ_GET_PARENT(cx, obj) == fp->blockChain);
                fp->blockChain = obj;
            }
          END_LITOPX_CASE(JSOP_ENTERBLOCK)

          BEGIN_CASE(JSOP_LEAVEBLOCKEXPR)
          BEGIN_CASE(JSOP_LEAVEBLOCK)
          {
            JSObject **chainp;

            /* Grab the result of the expression. */
            if (op == JSOP_LEAVEBLOCKEXPR)
                rval = FETCH_OPND(-1);

            chainp = &fp->blockChain;
            obj = *chainp;
            if (!obj) {
                chainp = &fp->scopeChain;
                obj = *chainp;

                /*
                 * This block was cloned, so clear its private data and sync
                 * its locals to their property slots.
                 */
                SAVE_SP_AND_PC(fp);
                ok = js_PutBlockObject(cx, obj);
                if (!ok)
                    goto out;
            }

            sp -= GET_UINT16(pc);
            JS_ASSERT(fp->spbase <= sp && sp <= fp->spbase + depth);

            /* Store the result into the topmost stack slot. */
            if (op == JSOP_LEAVEBLOCKEXPR)
                STORE_OPND(-1, rval);

            JS_ASSERT(OBJ_GET_CLASS(cx, obj) == &js_BlockClass);
            JS_ASSERT(op == JSOP_LEAVEBLOCKEXPR
                      ? fp->spbase + OBJ_BLOCK_DEPTH(cx, obj) == sp - 1
                      : fp->spbase + OBJ_BLOCK_DEPTH(cx, obj) == sp);

            *chainp = OBJ_GET_PARENT(cx, obj);
            JS_ASSERT(chainp != &fp->blockChain ||
                      !*chainp ||
                      OBJ_GET_CLASS(cx, *chainp) == &js_BlockClass);
          }
          END_CASE(JSOP_LEAVEBLOCK)

          BEGIN_CASE(JSOP_GETLOCAL)
            slot = GET_UINT16(pc);
            JS_ASSERT(slot < (uintN)depth);
            PUSH_OPND(fp->spbase[slot]);
            obj = NULL;
          END_CASE(JSOP_GETLOCAL)

          BEGIN_CASE(JSOP_SETLOCAL)
            slot = GET_UINT16(pc);
            JS_ASSERT(slot < (uintN)depth);
            vp = &fp->spbase[slot];
            GC_POKE(cx, *vp);
            *vp = FETCH_OPND(-1);
            obj = NULL;
          END_CASE(JSOP_SETLOCAL)

/* NB: This macro doesn't use JS_BEGIN_MACRO/JS_END_MACRO around its body. */
#define FAST_LOCAL_INCREMENT_OP(PRE,OPEQ,MINMAX)                              \
    slot = GET_UINT16(pc);                                                    \
    JS_ASSERT(slot < (uintN)depth);                                           \
    vp = fp->spbase + slot;                                                   \
    rval = *vp;                                                               \
    if (!JSVAL_IS_INT(rval) || rval == INT_TO_JSVAL(JSVAL_INT_##MINMAX))      \
        goto do_nonint_fast_incop;                                            \
    PRE = rval;                                                               \
    rval OPEQ 2;                                                              \
    *vp = rval;                                                               \
    PUSH_OPND(PRE)

          BEGIN_CASE(JSOP_INCLOCAL)
            FAST_LOCAL_INCREMENT_OP(rval, +=, MAX);
          END_CASE(JSOP_INCLOCAL)

          BEGIN_CASE(JSOP_DECLOCAL)
            FAST_LOCAL_INCREMENT_OP(rval, -=, MIN);
          END_CASE(JSOP_DECLOCAL)

          BEGIN_CASE(JSOP_LOCALINC)
            FAST_LOCAL_INCREMENT_OP(rtmp, +=, MAX);
          END_CASE(JSOP_LOCALINC)

          BEGIN_CASE(JSOP_LOCALDEC)
            FAST_LOCAL_INCREMENT_OP(rtmp, -=, MIN);
          END_CASE(JSOP_LOCALDEC)

#undef FAST_LOCAL_INCREMENT_OP

          EMPTY_CASE(JSOP_STARTITER)

          BEGIN_CASE(JSOP_ENDITER)
            JS_ASSERT(!JSVAL_IS_PRIMITIVE(sp[-1]));
            iterobj = JSVAL_TO_OBJECT(sp[-1]);

            /*
             * js_CloseNativeIterator checks whether the iterator is not
             * native, and also detects the case of a native iterator that
             * has already escaped, even though a for-in loop caused it to
             * be created.  See jsiter.c.
             */
            SAVE_SP_AND_PC(fp);
            js_CloseNativeIterator(cx, iterobj);
            *--sp = JSVAL_NULL;
          END_CASE(JSOP_ENDITER)

#if JS_HAS_GENERATORS
          BEGIN_CASE(JSOP_GENERATOR)
            pc += JSOP_GENERATOR_LENGTH;
            SAVE_SP_AND_PC(fp);
            obj = js_NewGenerator(cx, fp);
            if (!obj) {
                ok = JS_FALSE;
            } else {
                JS_ASSERT(!fp->callobj && !fp->argsobj);
                fp->rval = OBJECT_TO_JSVAL(obj);
            }
            goto out;

          BEGIN_CASE(JSOP_YIELD)
            ASSERT_NOT_THROWING(cx);
            if (fp->flags & JSFRAME_FILTERING) {
                /* FIXME: bug 309894 -- fix to eliminate this error. */
                JS_ReportErrorNumberUC(cx, js_GetErrorMessage, NULL,
                                       JSMSG_YIELD_FROM_FILTER);
                ok = JS_FALSE;
                goto out;
            }
            if (FRAME_TO_GENERATOR(fp)->state == JSGEN_CLOSING) {
                str = js_DecompileValueGenerator(cx, JSDVG_SEARCH_STACK,
                                                 fp->argv[-2], NULL);
                if (str) {
                    JS_ReportErrorNumberUC(cx, js_GetErrorMessage, NULL,
                                           JSMSG_BAD_GENERATOR_YIELD,
                                           JSSTRING_CHARS(str));
                }
                ok = JS_FALSE;
                goto out;
            }
            fp->rval = FETCH_OPND(-1);
            fp->flags |= JSFRAME_YIELDING;
            pc += JSOP_YIELD_LENGTH;
            SAVE_SP_AND_PC(fp);
            goto out;

          BEGIN_CASE(JSOP_ARRAYPUSH)
            slot = GET_UINT16(pc);
            JS_ASSERT(slot < (uintN)depth);
            lval = fp->spbase[slot];
            obj  = JSVAL_TO_OBJECT(lval);
            JS_ASSERT(OBJ_GET_CLASS(cx, obj) == &js_ArrayClass);
            rval = FETCH_OPND(-1);

            /* We know that the array is created with only a 'length' slot. */
            i = obj->map->freeslot - (JSSLOT_FREE(&js_ArrayClass) + 1);
            id = INT_TO_JSID(i);

            SAVE_SP_AND_PC(fp);
            ok = OBJ_SET_PROPERTY(cx, obj, id, &rval);
            if (!ok)
                goto out;
            --sp;
          END_CASE(JSOP_ARRAYPUSH)
#endif /* JS_HAS_GENERATORS */

#if !JS_HAS_GENERATORS
          L_JSOP_GENERATOR:
          L_JSOP_YIELD:
          L_JSOP_ARRAYPUSH:
#endif

#if !JS_HAS_DESTRUCTURING
          L_JSOP_FOREACHKEYVAL:
          L_JSOP_ENUMCONSTELEM:
#endif

#ifdef JS_THREADED_INTERP
          L_JSOP_BACKPATCH:
          L_JSOP_BACKPATCH_POP:
#else
          default:
#endif
          {
            char numBuf[12];
            JS_snprintf(numBuf, sizeof numBuf, "%d", op);
            JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL,
                                 JSMSG_BAD_BYTECODE, numBuf);
            ok = JS_FALSE;
            goto out;
          }

#ifndef JS_THREADED_INTERP

        } /* switch (op) */

    advance_pc:
        pc += len;

#ifdef DEBUG
        if (tracefp) {
            intN ndefs, n;
            jsval *siter;

            ndefs = js_CodeSpec[op].ndefs;
            if (ndefs) {
                SAVE_SP_AND_PC(fp);
                if (op == JSOP_FORELEM && sp[-1] == JSVAL_FALSE)
                    --ndefs;
                for (n = -ndefs; n < 0; n++) {
                    str = js_DecompileValueGenerator(cx, n, sp[n], NULL);
                    if (str) {
                        fprintf(tracefp, "%s %s",
                                (n == -ndefs) ? "  output:" : ",",
                                JS_GetStringBytes(str));
                    }
                }
                fprintf(tracefp, " @ %d\n", sp - fp->spbase);
            }
            fprintf(tracefp, "  stack: ");
            for (siter = fp->spbase; siter < sp; siter++) {
                str = js_ValueToSource(cx, *siter);
                fprintf(tracefp, "%s ",
                        str ? JS_GetStringBytes(str) : "<null>");
            }
            fputc('\n', tracefp);
        }
#endif /* DEBUG */
    }
#endif /* !JS_THREADED_INTERP */

out:
    if (!ok) {
        /*
         * Has an exception been raised?  Also insist that we are not in an
         * XML filtering predicate expression, to avoid catching exceptions
         * within the filtering predicate, such as this example taken from
         * tests/e4x/Regress/regress-301596.js:
         *
         *    try {
         *        <xml/>.(@a == 1);
         *        throw 5;
         *    } catch (e) {
         *    }
         *
         * The inner interpreter activation executing the predicate bytecode
         * will throw "reference to undefined XML name @a" (or 5, in older
         * versions that followed the first edition of ECMA-357 and evaluated
         * unbound identifiers to undefined), and the exception must not be
         * caught until control unwinds to the outer interpreter activation.
         *
         * Otherwise, the wrong stack depth will be restored by JSOP_SETSP,
         * and the catch will move into the filtering predicate expression,
         * leading to double catch execution if it rethrows.
         *
         * FIXME: https://bugzilla.mozilla.org/show_bug.cgi?id=309894
         */
        if (cx->throwing && !(fp->flags & JSFRAME_FILTERING)) {
            /*
             * Call debugger throw hook if set (XXX thread safety?).
             */
            JSTrapHandler handler = rt->throwHook;
            if (handler) {
                SAVE_SP_AND_PC(fp);
                switch (handler(cx, script, pc, &rval, rt->throwHookData)) {
                  case JSTRAP_ERROR:
                    cx->throwing = JS_FALSE;
                    goto no_catch;
                  case JSTRAP_RETURN:
                    ok = JS_TRUE;
                    cx->throwing = JS_FALSE;
                    fp->rval = rval;
                    goto no_catch;
                  case JSTRAP_THROW:
                    cx->exception = rval;
                  case JSTRAP_CONTINUE:
                  default:;
                }
                LOAD_INTERRUPT_HANDLER(rt);
            }

            /*
             * Look for a try block in script that can catch this exception.
             */
#if JS_HAS_GENERATORS
            if (JS_LIKELY(cx->exception != JSVAL_ARETURN)) {
                SCRIPT_FIND_CATCH_START(script, pc, pc);
                if (!pc)
                    goto no_catch;
            } else {
                pc = js_FindFinallyHandler(script, pc);
                if (!pc) {
                    cx->throwing = JS_FALSE;
                    ok = JS_TRUE;
                    fp->rval = JSVAL_VOID;
                    goto no_catch;
                }
            }
#else
            SCRIPT_FIND_CATCH_START(script, pc, pc);
            if (!pc)
                goto no_catch;
#endif

            /* Don't clear cx->throwing to save cx->exception from GC. */
            len = 0;
            ok = JS_TRUE;
            DO_NEXT_OP(len);
        }
no_catch:;
    }

    /*
     * Check whether control fell off the end of a lightweight function, or an
     * exception thrown under such a function was not caught by it.  If so, go
     * to the inline code under JSOP_RETURN.
     */
    if (inlineCallCount)
        goto inline_return;

    /*
     * Reset sp before freeing stack slots, because our caller may GC soon.
     * Clear spbase to indicate that we've popped the 2 * depth operand slots.
     * Restore the previous frame's execution state.
     */
    if (JS_LIKELY(mark != NULL)) {
        /* If fp has blocks on its scope chain, home their locals now. */
        if (fp->flags & JSFRAME_POP_BLOCKS) {
            SAVE_SP_AND_PC(fp);
            ok &= PutBlockObjects(cx, fp);
        }

        fp->sp = fp->spbase;
        fp->spbase = NULL;
        js_FreeRawStack(cx, mark);
    } else {
        SAVE_SP(fp);
    }

out2:
    if (cx->version == currentVersion && currentVersion != originalVersion)
        js_SetVersion(cx, originalVersion);
    cx->interpLevel--;
    return ok;

atom_not_defined:
    {
        const char *printable = js_AtomToPrintableString(cx, atom);
        if (printable)
            js_ReportIsNotDefined(cx, printable);
        ok = JS_FALSE;
        goto out;
    }
}
