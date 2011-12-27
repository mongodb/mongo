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
 * JS debugging API.
 */
#include "jsstddef.h"
#include <string.h>
#include "jstypes.h"
#include "jsutil.h" /* Added by JSIFY */
#include "jsclist.h"
#include "jsapi.h"
#include "jscntxt.h"
#include "jsconfig.h"
#include "jsdbgapi.h"
#include "jsfun.h"
#include "jsgc.h"
#include "jsinterp.h"
#include "jslock.h"
#include "jsobj.h"
#include "jsopcode.h"
#include "jsscope.h"
#include "jsscript.h"
#include "jsstr.h"

typedef struct JSTrap {
    JSCList         links;
    JSScript        *script;
    jsbytecode      *pc;
    JSOp            op;
    JSTrapHandler   handler;
    void            *closure;
} JSTrap;

static JSTrap *
FindTrap(JSRuntime *rt, JSScript *script, jsbytecode *pc)
{
    JSTrap *trap;

    for (trap = (JSTrap *)rt->trapList.next;
         trap != (JSTrap *)&rt->trapList;
         trap = (JSTrap *)trap->links.next) {
        if (trap->script == script && trap->pc == pc)
            return trap;
    }
    return NULL;
}

void
js_PatchOpcode(JSContext *cx, JSScript *script, jsbytecode *pc, JSOp op)
{
    JSTrap *trap;

    trap = FindTrap(cx->runtime, script, pc);
    if (trap)
        trap->op = op;
    else
        *pc = (jsbytecode)op;
}

JS_PUBLIC_API(JSBool)
JS_SetTrap(JSContext *cx, JSScript *script, jsbytecode *pc,
           JSTrapHandler handler, void *closure)
{
    JSRuntime *rt;
    JSTrap *trap;

    rt = cx->runtime;
    trap = FindTrap(rt, script, pc);
    if (trap) {
        JS_ASSERT(trap->script == script && trap->pc == pc);
        JS_ASSERT(*pc == JSOP_TRAP);
    } else {
        trap = (JSTrap *) JS_malloc(cx, sizeof *trap);
        if (!trap || !js_AddRoot(cx, &trap->closure, "trap->closure")) {
            if (trap)
                JS_free(cx, trap);
            return JS_FALSE;
        }
        JS_APPEND_LINK(&trap->links, &rt->trapList);
        trap->script = script;
        trap->pc = pc;
        trap->op = (JSOp)*pc;
        *pc = JSOP_TRAP;
    }
    trap->handler = handler;
    trap->closure = closure;
    return JS_TRUE;
}

JS_PUBLIC_API(JSOp)
JS_GetTrapOpcode(JSContext *cx, JSScript *script, jsbytecode *pc)
{
    JSTrap *trap;

    trap = FindTrap(cx->runtime, script, pc);
    if (!trap) {
        JS_ASSERT(0);   /* XXX can't happen */
        return JSOP_LIMIT;
    }
    return trap->op;
}

static void
DestroyTrap(JSContext *cx, JSTrap *trap)
{
    JS_REMOVE_LINK(&trap->links);
    *trap->pc = (jsbytecode)trap->op;
    js_RemoveRoot(cx->runtime, &trap->closure);
    JS_free(cx, trap);
}

JS_PUBLIC_API(void)
JS_ClearTrap(JSContext *cx, JSScript *script, jsbytecode *pc,
             JSTrapHandler *handlerp, void **closurep)
{
    JSTrap *trap;

    trap = FindTrap(cx->runtime, script, pc);
    if (handlerp)
        *handlerp = trap ? trap->handler : NULL;
    if (closurep)
        *closurep = trap ? trap->closure : NULL;
    if (trap)
        DestroyTrap(cx, trap);
}

JS_PUBLIC_API(void)
JS_ClearScriptTraps(JSContext *cx, JSScript *script)
{
    JSRuntime *rt;
    JSTrap *trap, *next;

    rt = cx->runtime;
    for (trap = (JSTrap *)rt->trapList.next;
         trap != (JSTrap *)&rt->trapList;
         trap = next) {
        next = (JSTrap *)trap->links.next;
        if (trap->script == script)
            DestroyTrap(cx, trap);
    }
}

JS_PUBLIC_API(void)
JS_ClearAllTraps(JSContext *cx)
{
    JSRuntime *rt;
    JSTrap *trap, *next;

    rt = cx->runtime;
    for (trap = (JSTrap *)rt->trapList.next;
         trap != (JSTrap *)&rt->trapList;
         trap = next) {
        next = (JSTrap *)trap->links.next;
        DestroyTrap(cx, trap);
    }
}

JS_PUBLIC_API(JSTrapStatus)
JS_HandleTrap(JSContext *cx, JSScript *script, jsbytecode *pc, jsval *rval)
{
    JSTrap *trap;
    JSTrapStatus status;
    jsint op;

    trap = FindTrap(cx->runtime, script, pc);
    if (!trap) {
        JS_ASSERT(0);   /* XXX can't happen */
        return JSTRAP_ERROR;
    }
    /*
     * It's important that we not use 'trap->' after calling the callback --
     * the callback might remove the trap!
     */
    op = (jsint)trap->op;
    status = trap->handler(cx, script, pc, rval, trap->closure);
    if (status == JSTRAP_CONTINUE) {
        /* By convention, return the true op to the interpreter in rval. */
        *rval = INT_TO_JSVAL(op);
    }
    return status;
}

JS_PUBLIC_API(JSBool)
JS_SetInterrupt(JSRuntime *rt, JSTrapHandler handler, void *closure)
{
    rt->interruptHandler = handler;
    rt->interruptHandlerData = closure;
    return JS_TRUE;
}

JS_PUBLIC_API(JSBool)
JS_ClearInterrupt(JSRuntime *rt, JSTrapHandler *handlerp, void **closurep)
{
    if (handlerp)
        *handlerp = (JSTrapHandler)rt->interruptHandler;
    if (closurep)
        *closurep = rt->interruptHandlerData;
    rt->interruptHandler = 0;
    rt->interruptHandlerData = 0;
    return JS_TRUE;
}

/************************************************************************/

typedef struct JSWatchPoint {
    JSCList             links;
    JSObject            *object;        /* weak link, see js_FinalizeObject */
    JSScopeProperty     *sprop;
    JSPropertyOp        setter;
    JSWatchPointHandler handler;
    void                *closure;
    uintN               flags;
} JSWatchPoint;

#define JSWP_LIVE       0x1             /* live because set and not cleared */
#define JSWP_HELD       0x2             /* held while running handler/setter */

static JSBool
DropWatchPoint(JSContext *cx, JSWatchPoint *wp, uintN flag)
{
    JSBool ok;
    JSScopeProperty *sprop;
    JSObject *pobj;
    JSProperty *prop;
    JSPropertyOp setter;

    ok = JS_TRUE;
    wp->flags &= ~flag;
    if (wp->flags != 0)
        return JS_TRUE;

    /*
     * Remove wp from the list, then if there are no other watchpoints for
     * wp->sprop in any scope, restore wp->sprop->setter from wp.
     */
    JS_REMOVE_LINK(&wp->links);
    sprop = wp->sprop;

    /*
     * If js_ChangeNativePropertyAttrs fails, propagate failure after removing
     * wp->closure's root and freeing wp.
     */
    setter = js_GetWatchedSetter(cx->runtime, NULL, sprop);
    if (!setter) {
        ok = js_LookupProperty(cx, wp->object, sprop->id, &pobj, &prop);

        /*
         * If the property wasn't found on wp->object or didn't exist, then
         * someone else has dealt with this sprop, and we don't need to change
         * the property attributes.
         */
        if (ok && prop) {
            if (pobj == wp->object) {
                JS_ASSERT(OBJ_SCOPE(pobj)->object == pobj);

                sprop = js_ChangeScopePropertyAttrs(cx, OBJ_SCOPE(pobj), sprop,
                                                    0, sprop->attrs,
                                                    sprop->getter,
                                                    wp->setter);
                if (!sprop)
                    ok = JS_FALSE;
            }
            OBJ_DROP_PROPERTY(cx, pobj, prop);
        }
    }

    js_RemoveRoot(cx->runtime, &wp->closure);
    JS_free(cx, wp);
    return ok;
}

void
js_MarkWatchPoints(JSContext *cx)
{
    JSRuntime *rt;
    JSWatchPoint *wp;

    rt = cx->runtime;
    for (wp = (JSWatchPoint *)rt->watchPointList.next;
         wp != (JSWatchPoint *)&rt->watchPointList;
         wp = (JSWatchPoint *)wp->links.next) {
        MARK_SCOPE_PROPERTY(cx, wp->sprop);
        if (wp->sprop->attrs & JSPROP_SETTER)
            JS_MarkGCThing(cx, wp->setter, "wp->setter", NULL);
    }
}

static JSWatchPoint *
FindWatchPoint(JSRuntime *rt, JSScope *scope, jsid id)
{
    JSWatchPoint *wp;

    for (wp = (JSWatchPoint *)rt->watchPointList.next;
         wp != (JSWatchPoint *)&rt->watchPointList;
         wp = (JSWatchPoint *)wp->links.next) {
        if (wp->object == scope->object && wp->sprop->id == id)
            return wp;
    }
    return NULL;
}

JSScopeProperty *
js_FindWatchPoint(JSRuntime *rt, JSScope *scope, jsid id)
{
    JSWatchPoint *wp;

    wp = FindWatchPoint(rt, scope, id);
    if (!wp)
        return NULL;
    return wp->sprop;
}

JSPropertyOp
js_GetWatchedSetter(JSRuntime *rt, JSScope *scope,
                    const JSScopeProperty *sprop)
{
    JSWatchPoint *wp;

    for (wp = (JSWatchPoint *)rt->watchPointList.next;
         wp != (JSWatchPoint *)&rt->watchPointList;
         wp = (JSWatchPoint *)wp->links.next) {
        if ((!scope || wp->object == scope->object) && wp->sprop == sprop)
            return wp->setter;
    }
    return NULL;
}

JSBool JS_DLL_CALLBACK
js_watch_set(JSContext *cx, JSObject *obj, jsval id, jsval *vp)
{
    JSRuntime *rt;
    JSWatchPoint *wp;
    JSScopeProperty *sprop;
    jsval propid, userid;
    JSScope *scope;
    JSBool ok;

    rt = cx->runtime;
    for (wp = (JSWatchPoint *)rt->watchPointList.next;
         wp != (JSWatchPoint *)&rt->watchPointList;
         wp = (JSWatchPoint *)wp->links.next) {
        sprop = wp->sprop;
        if (wp->object == obj && SPROP_USERID(sprop) == id &&
            !(wp->flags & JSWP_HELD)) {
            wp->flags |= JSWP_HELD;

            JS_LOCK_OBJ(cx, obj);
            propid = ID_TO_VALUE(sprop->id);
            userid = (sprop->flags & SPROP_HAS_SHORTID)
                     ? INT_TO_JSVAL(sprop->shortid)
                     : propid;
            scope = OBJ_SCOPE(obj);
            JS_UNLOCK_OBJ(cx, obj);

            /* NB: wp is held, so we can safely dereference it still. */
            ok = wp->handler(cx, obj, propid,
                             SPROP_HAS_VALID_SLOT(sprop, scope)
                             ? OBJ_GET_SLOT(cx, obj, sprop->slot)
                             : JSVAL_VOID,
                             vp, wp->closure);
            if (ok) {
                /*
                 * Create a pseudo-frame for the setter invocation so that any
                 * stack-walking security code under the setter will correctly
                 * identify the guilty party.  So that the watcher appears to
                 * be active to obj_eval and other such code, point frame.pc
                 * at the JSOP_STOP at the end of the script.
                 */
                JSObject *closure;
                JSClass *clasp;
                JSFunction *fun;
                JSScript *script;
                uintN nslots;
                jsval smallv[5];
                jsval *argv;
                JSStackFrame frame;

                closure = (JSObject *) wp->closure;
                clasp = OBJ_GET_CLASS(cx, closure);
                if (clasp == &js_FunctionClass) {
                    fun = (JSFunction *) JS_GetPrivate(cx, closure);
                    script = FUN_SCRIPT(fun);
                } else if (clasp == &js_ScriptClass) {
                    fun = NULL;
                    script = (JSScript *) JS_GetPrivate(cx, closure);
                } else {
                    fun = NULL;
                    script = NULL;
                }

                nslots = 2;
                if (fun) {
                    nslots += fun->nargs;
                    if (FUN_NATIVE(fun))
                        nslots += fun->u.n.extra;
                }

                if (nslots <= JS_ARRAY_LENGTH(smallv)) {
                    argv = smallv;
                } else {
                    argv = JS_malloc(cx, nslots * sizeof(jsval));
                    if (!argv) {
                        DropWatchPoint(cx, wp, JSWP_HELD);
                        return JS_FALSE;
                    }
                }

                argv[0] = OBJECT_TO_JSVAL(closure);
                argv[1] = JSVAL_NULL;
                memset(argv + 2, 0, (nslots - 2) * sizeof(jsval));

                memset(&frame, 0, sizeof(frame));
                frame.script = script;
                if (script) {
                    JS_ASSERT(script->length >= JSOP_STOP_LENGTH);
                    frame.pc = script->code + script->length
                             - JSOP_STOP_LENGTH;
                }
                frame.fun = fun;
                frame.argv = argv + 2;
                frame.down = cx->fp;
                frame.scopeChain = OBJ_GET_PARENT(cx, closure);

                cx->fp = &frame;
                ok = !wp->setter ||
                     ((sprop->attrs & JSPROP_SETTER)
                      ? js_InternalCall(cx, obj, OBJECT_TO_JSVAL(wp->setter),
                                        1, vp, vp)
                      : wp->setter(cx, OBJ_THIS_OBJECT(cx, obj), userid, vp));
                cx->fp = frame.down;
                if (argv != smallv)
                    JS_free(cx, argv);
            }
            return DropWatchPoint(cx, wp, JSWP_HELD) && ok;
        }
    }
    return JS_TRUE;
}

JSBool JS_DLL_CALLBACK
js_watch_set_wrapper(JSContext *cx, JSObject *obj, uintN argc, jsval *argv,
                     jsval *rval)
{
    JSObject *funobj;
    JSFunction *wrapper;
    jsval userid;

    funobj = JSVAL_TO_OBJECT(argv[-2]);
    JS_ASSERT(OBJ_GET_CLASS(cx, funobj) == &js_FunctionClass);
    wrapper = (JSFunction *) JS_GetPrivate(cx, funobj);
    userid = ATOM_KEY(wrapper->atom);
    *rval = argv[0];
    return js_watch_set(cx, obj, userid, rval);
}

JSPropertyOp
js_WrapWatchedSetter(JSContext *cx, jsid id, uintN attrs, JSPropertyOp setter)
{
    JSAtom *atom;
    JSFunction *wrapper;

    if (!(attrs & JSPROP_SETTER))
        return &js_watch_set;   /* & to silence schoolmarmish MSVC */

    if (JSID_IS_ATOM(id)) {
        atom = JSID_TO_ATOM(id);
    } else if (JSID_IS_INT(id)) {
        atom = js_AtomizeInt(cx, JSID_TO_INT(id), 0);
        if (!atom)
            return NULL;
    } else {
        atom = NULL;
    }
    wrapper = js_NewFunction(cx, NULL, js_watch_set_wrapper, 1, 0,
                             OBJ_GET_PARENT(cx, (JSObject *)setter),
                             atom);
    if (!wrapper)
        return NULL;
    return (JSPropertyOp) wrapper->object;
}

JS_PUBLIC_API(JSBool)
JS_SetWatchPoint(JSContext *cx, JSObject *obj, jsval id,
                 JSWatchPointHandler handler, void *closure)
{
    JSAtom *atom;
    jsid propid;
    JSObject *pobj;
    JSProperty *prop;
    JSScopeProperty *sprop;
    JSRuntime *rt;
    JSBool ok;
    JSWatchPoint *wp;
    JSPropertyOp watcher;

    if (!OBJ_IS_NATIVE(obj)) {
        JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_CANT_WATCH,
                             OBJ_GET_CLASS(cx, obj)->name);
        return JS_FALSE;
    }

    if (JSVAL_IS_INT(id)) {
        propid = INT_JSVAL_TO_JSID(id);
        atom = NULL;
    } else {
        atom = js_ValueToStringAtom(cx, id);
        if (!atom)
            return JS_FALSE;
        propid = ATOM_TO_JSID(atom);
    }

    if (!js_LookupProperty(cx, obj, propid, &pobj, &prop))
        return JS_FALSE;
    sprop = (JSScopeProperty *) prop;
    rt = cx->runtime;
    if (!sprop) {
        /* Check for a deleted symbol watchpoint, which holds its property. */
        sprop = js_FindWatchPoint(rt, OBJ_SCOPE(obj), propid);
        if (!sprop) {
            /* Make a new property in obj so we can watch for the first set. */
            if (!js_DefineProperty(cx, obj, propid, JSVAL_VOID,
                                   NULL, NULL, JSPROP_ENUMERATE,
                                   &prop)) {
                return JS_FALSE;
            }
            sprop = (JSScopeProperty *) prop;
        }
    } else if (pobj != obj) {
        /* Clone the prototype property so we can watch the right object. */
        jsval value;
        JSPropertyOp getter, setter;
        uintN attrs, flags;
        intN shortid;

        if (OBJ_IS_NATIVE(pobj)) {
            value = SPROP_HAS_VALID_SLOT(sprop, OBJ_SCOPE(pobj))
                    ? LOCKED_OBJ_GET_SLOT(pobj, sprop->slot)
                    : JSVAL_VOID;
            getter = sprop->getter;
            setter = sprop->setter;
            attrs = sprop->attrs;
            flags = sprop->flags;
            shortid = sprop->shortid;
        } else {
            if (!OBJ_GET_PROPERTY(cx, pobj, id, &value) ||
                !OBJ_GET_ATTRIBUTES(cx, pobj, id, prop, &attrs)) {
                OBJ_DROP_PROPERTY(cx, pobj, prop);
                return JS_FALSE;
            }
            getter = setter = NULL;
            flags = 0;
            shortid = 0;
        }
        OBJ_DROP_PROPERTY(cx, pobj, prop);

        /* Recall that obj is native, whether or not pobj is native. */
        if (!js_DefineNativeProperty(cx, obj, propid, value, getter, setter,
                                     attrs, flags, shortid, &prop)) {
            return JS_FALSE;
        }
        sprop = (JSScopeProperty *) prop;
    }

    /*
     * At this point, prop/sprop exists in obj, obj is locked, and we must
     * OBJ_DROP_PROPERTY(cx, obj, prop) before returning.
     */
    ok = JS_TRUE;
    wp = FindWatchPoint(rt, OBJ_SCOPE(obj), propid);
    if (!wp) {
        watcher = js_WrapWatchedSetter(cx, propid, sprop->attrs, sprop->setter);
        if (!watcher) {
            ok = JS_FALSE;
            goto out;
        }

        wp = (JSWatchPoint *) JS_malloc(cx, sizeof *wp);
        if (!wp) {
            ok = JS_FALSE;
            goto out;
        }
        wp->handler = NULL;
        wp->closure = NULL;
        ok = js_AddRoot(cx, &wp->closure, "wp->closure");
        if (!ok) {
            JS_free(cx, wp);
            goto out;
        }
        wp->object = obj;
        JS_ASSERT(sprop->setter != js_watch_set || pobj != obj);
        wp->setter = sprop->setter;
        wp->flags = JSWP_LIVE;

        /* XXXbe nest in obj lock here */
        sprop = js_ChangeNativePropertyAttrs(cx, obj, sprop, 0, sprop->attrs,
                                             sprop->getter, watcher);
        if (!sprop) {
            /* Self-link so DropWatchPoint can JS_REMOVE_LINK it. */
            JS_INIT_CLIST(&wp->links);
            DropWatchPoint(cx, wp, JSWP_LIVE);
            ok = JS_FALSE;
            goto out;
        }
        wp->sprop = sprop;

        /*
         * Now that wp is fully initialized, append it to rt's wp list.
         * Because obj is locked we know that no other thread could have added
         * a watchpoint for (obj, propid).
         */
        JS_ASSERT(!FindWatchPoint(rt, OBJ_SCOPE(obj), propid));
        JS_APPEND_LINK(&wp->links, &rt->watchPointList);
    }
    wp->handler = handler;
    wp->closure = closure;

out:
    OBJ_DROP_PROPERTY(cx, obj, prop);
    return ok;
}

JS_PUBLIC_API(JSBool)
JS_ClearWatchPoint(JSContext *cx, JSObject *obj, jsval id,
                   JSWatchPointHandler *handlerp, void **closurep)
{
    JSRuntime *rt;
    JSWatchPoint *wp;

    rt = cx->runtime;
    for (wp = (JSWatchPoint *)rt->watchPointList.next;
         wp != (JSWatchPoint *)&rt->watchPointList;
         wp = (JSWatchPoint *)wp->links.next) {
        if (wp->object == obj && SPROP_USERID(wp->sprop) == id) {
            if (handlerp)
                *handlerp = wp->handler;
            if (closurep)
                *closurep = wp->closure;
            return DropWatchPoint(cx, wp, JSWP_LIVE);
        }
    }
    if (handlerp)
        *handlerp = NULL;
    if (closurep)
        *closurep = NULL;
    return JS_TRUE;
}

JS_PUBLIC_API(JSBool)
JS_ClearWatchPointsForObject(JSContext *cx, JSObject *obj)
{
    JSRuntime *rt;
    JSWatchPoint *wp, *next;

    rt = cx->runtime;
    for (wp = (JSWatchPoint *)rt->watchPointList.next;
         wp != (JSWatchPoint *)&rt->watchPointList;
         wp = next) {
        next = (JSWatchPoint *)wp->links.next;
        if (wp->object == obj && !DropWatchPoint(cx, wp, JSWP_LIVE))
            return JS_FALSE;
    }
    return JS_TRUE;
}

JS_PUBLIC_API(JSBool)
JS_ClearAllWatchPoints(JSContext *cx)
{
    JSRuntime *rt;
    JSWatchPoint *wp, *next;

    rt = cx->runtime;
    for (wp = (JSWatchPoint *)rt->watchPointList.next;
         wp != (JSWatchPoint *)&rt->watchPointList;
         wp = next) {
        next = (JSWatchPoint *)wp->links.next;
        if (!DropWatchPoint(cx, wp, JSWP_LIVE))
            return JS_FALSE;
    }
    return JS_TRUE;
}

/************************************************************************/

JS_PUBLIC_API(uintN)
JS_PCToLineNumber(JSContext *cx, JSScript *script, jsbytecode *pc)
{
    return js_PCToLineNumber(cx, script, pc);
}

JS_PUBLIC_API(jsbytecode *)
JS_LineNumberToPC(JSContext *cx, JSScript *script, uintN lineno)
{
    return js_LineNumberToPC(script, lineno);
}

JS_PUBLIC_API(JSScript *)
JS_GetFunctionScript(JSContext *cx, JSFunction *fun)
{
    return FUN_SCRIPT(fun);
}

JS_PUBLIC_API(JSNative)
JS_GetFunctionNative(JSContext *cx, JSFunction *fun)
{
    return FUN_NATIVE(fun);
}

JS_PUBLIC_API(JSPrincipals *)
JS_GetScriptPrincipals(JSContext *cx, JSScript *script)
{
    return script->principals;
}

/************************************************************************/

/*
 *  Stack Frame Iterator
 */
JS_PUBLIC_API(JSStackFrame *)
JS_FrameIterator(JSContext *cx, JSStackFrame **iteratorp)
{
    *iteratorp = (*iteratorp == NULL) ? cx->fp : (*iteratorp)->down;
    return *iteratorp;
}

JS_PUBLIC_API(JSScript *)
JS_GetFrameScript(JSContext *cx, JSStackFrame *fp)
{
    return fp->script;
}

JS_PUBLIC_API(jsbytecode *)
JS_GetFramePC(JSContext *cx, JSStackFrame *fp)
{
    return fp->pc;
}

JS_PUBLIC_API(JSStackFrame *)
JS_GetScriptedCaller(JSContext *cx, JSStackFrame *fp)
{
    if (!fp)
        fp = cx->fp;
    while ((fp = fp->down) != NULL) {
        if (fp->script)
            return fp;
    }
    return NULL;
}

JS_PUBLIC_API(JSPrincipals *)
JS_StackFramePrincipals(JSContext *cx, JSStackFrame *fp)
{
    if (fp->fun) {
        JSRuntime *rt = cx->runtime;

        if (rt->findObjectPrincipals) {
            JSObject *callee = JSVAL_TO_OBJECT(fp->argv[-2]);

            if (fp->fun->object != callee)
                return rt->findObjectPrincipals(cx, callee);
            /* FALL THROUGH */
        }
    }
    if (fp->script)
        return fp->script->principals;
    return NULL;
}

JS_PUBLIC_API(JSPrincipals *)
JS_EvalFramePrincipals(JSContext *cx, JSStackFrame *fp, JSStackFrame *caller)
{
    JSRuntime *rt;
    JSObject *callee;
    JSPrincipals *principals, *callerPrincipals;

    rt = cx->runtime;
    if (rt->findObjectPrincipals) {
        callee = JSVAL_TO_OBJECT(fp->argv[-2]);
        principals = rt->findObjectPrincipals(cx, callee);
    } else {
        principals = NULL;
    }
    if (!caller)
        return principals;
    callerPrincipals = JS_StackFramePrincipals(cx, caller);
    return (callerPrincipals && principals &&
            callerPrincipals->subsume(callerPrincipals, principals))
           ? principals
           : callerPrincipals;
}

JS_PUBLIC_API(void *)
JS_GetFrameAnnotation(JSContext *cx, JSStackFrame *fp)
{
    if (fp->annotation && fp->script) {
        JSPrincipals *principals = JS_StackFramePrincipals(cx, fp);

        if (principals && principals->globalPrivilegesEnabled(cx, principals)) {
            /*
             * Give out an annotation only if privileges have not been revoked
             * or disabled globally.
             */
            return fp->annotation;
        }
    }

    return NULL;
}

JS_PUBLIC_API(void)
JS_SetFrameAnnotation(JSContext *cx, JSStackFrame *fp, void *annotation)
{
    fp->annotation = annotation;
}

JS_PUBLIC_API(void *)
JS_GetFramePrincipalArray(JSContext *cx, JSStackFrame *fp)
{
    JSPrincipals *principals;

    principals = JS_StackFramePrincipals(cx, fp);
    if (!principals)
        return NULL;
    return principals->getPrincipalArray(cx, principals);
}

JS_PUBLIC_API(JSBool)
JS_IsNativeFrame(JSContext *cx, JSStackFrame *fp)
{
    return !fp->script;
}

/* this is deprecated, use JS_GetFrameScopeChain instead */
JS_PUBLIC_API(JSObject *)
JS_GetFrameObject(JSContext *cx, JSStackFrame *fp)
{
    return fp->scopeChain;
}

JS_PUBLIC_API(JSObject *)
JS_GetFrameScopeChain(JSContext *cx, JSStackFrame *fp)
{
    /* Force creation of argument and call objects if not yet created */
    (void) JS_GetFrameCallObject(cx, fp);
    return js_GetScopeChain(cx, fp);
}

JS_PUBLIC_API(JSObject *)
JS_GetFrameCallObject(JSContext *cx, JSStackFrame *fp)
{
    if (! fp->fun)
        return NULL;

    /* Force creation of argument object if not yet created */
    (void) js_GetArgsObject(cx, fp);

    /*
     * XXX ill-defined: null return here means error was reported, unlike a
     *     null returned above or in the #else
     */
    return js_GetCallObject(cx, fp, NULL);
}


JS_PUBLIC_API(JSObject *)
JS_GetFrameThis(JSContext *cx, JSStackFrame *fp)
{
    return fp->thisp;
}

JS_PUBLIC_API(JSFunction *)
JS_GetFrameFunction(JSContext *cx, JSStackFrame *fp)
{
    return fp->fun;
}

JS_PUBLIC_API(JSObject *)
JS_GetFrameFunctionObject(JSContext *cx, JSStackFrame *fp)
{
    return fp->argv && fp->fun ? JSVAL_TO_OBJECT(fp->argv[-2]) : NULL;
}

JS_PUBLIC_API(JSBool)
JS_IsConstructorFrame(JSContext *cx, JSStackFrame *fp)
{
    return (fp->flags & JSFRAME_CONSTRUCTING) != 0;
}

JS_PUBLIC_API(JSObject *)
JS_GetFrameCalleeObject(JSContext *cx, JSStackFrame *fp)
{
    return fp->argv ? JSVAL_TO_OBJECT(fp->argv[-2]) : NULL;
}

JS_PUBLIC_API(JSBool)
JS_IsDebuggerFrame(JSContext *cx, JSStackFrame *fp)
{
    return (fp->flags & JSFRAME_DEBUGGER) != 0;
}

JS_PUBLIC_API(jsval)
JS_GetFrameReturnValue(JSContext *cx, JSStackFrame *fp)
{
    return fp->rval;
}

JS_PUBLIC_API(void)
JS_SetFrameReturnValue(JSContext *cx, JSStackFrame *fp, jsval rval)
{
    fp->rval = rval;
}

/************************************************************************/

JS_PUBLIC_API(const char *)
JS_GetScriptFilename(JSContext *cx, JSScript *script)
{
    return script->filename;
}

JS_PUBLIC_API(uintN)
JS_GetScriptBaseLineNumber(JSContext *cx, JSScript *script)
{
    return script->lineno;
}

JS_PUBLIC_API(uintN)
JS_GetScriptLineExtent(JSContext *cx, JSScript *script)
{
    return js_GetScriptLineExtent(script);
}

JS_PUBLIC_API(JSVersion)
JS_GetScriptVersion(JSContext *cx, JSScript *script)
{
    return script->version & JSVERSION_MASK;
}

/***************************************************************************/

JS_PUBLIC_API(void)
JS_SetNewScriptHook(JSRuntime *rt, JSNewScriptHook hook, void *callerdata)
{
    rt->newScriptHook = hook;
    rt->newScriptHookData = callerdata;
}

JS_PUBLIC_API(void)
JS_SetDestroyScriptHook(JSRuntime *rt, JSDestroyScriptHook hook,
                        void *callerdata)
{
    rt->destroyScriptHook = hook;
    rt->destroyScriptHookData = callerdata;
}

/***************************************************************************/

JS_PUBLIC_API(JSBool)
JS_EvaluateUCInStackFrame(JSContext *cx, JSStackFrame *fp,
                          const jschar *chars, uintN length,
                          const char *filename, uintN lineno,
                          jsval *rval)
{
    JSObject *scobj;
    uint32 flags, options;
    JSScript *script;
    JSBool ok;

    scobj = JS_GetFrameScopeChain(cx, fp);
    if (!scobj)
        return JS_FALSE;

    /*
     * XXX Hack around ancient compiler API to propagate the JSFRAME_SPECIAL
     * flags to the code generator (see js_EmitTree's TOK_SEMI case).
     */
    flags = fp->flags;
    fp->flags |= JSFRAME_DEBUGGER | JSFRAME_EVAL;
    options = cx->options;
    cx->options = options | JSOPTION_COMPILE_N_GO;
    script = JS_CompileUCScriptForPrincipals(cx, scobj,
                                             JS_StackFramePrincipals(cx, fp),
                                             chars, length, filename, lineno);
    fp->flags = flags;
    cx->options = options;
    if (!script)
        return JS_FALSE;

    ok = js_Execute(cx, scobj, script, fp, JSFRAME_DEBUGGER | JSFRAME_EVAL,
                    rval);
    js_DestroyScript(cx, script);
    return ok;
}

JS_PUBLIC_API(JSBool)
JS_EvaluateInStackFrame(JSContext *cx, JSStackFrame *fp,
                        const char *bytes, uintN length,
                        const char *filename, uintN lineno,
                        jsval *rval)
{
    jschar *chars;
    JSBool ok;
    size_t len = length;

    chars = js_InflateString(cx, bytes, &len);
    if (!chars)
        return JS_FALSE;
    length = (uintN) len;
    ok = JS_EvaluateUCInStackFrame(cx, fp, chars, length, filename, lineno,
                                   rval);
    JS_free(cx, chars);

    return ok;
}

/************************************************************************/

/* XXXbe this all needs to be reworked to avoid requiring JSScope types. */

JS_PUBLIC_API(JSScopeProperty *)
JS_PropertyIterator(JSObject *obj, JSScopeProperty **iteratorp)
{
    JSScopeProperty *sprop;
    JSScope *scope;

    sprop = *iteratorp;
    scope = OBJ_SCOPE(obj);

    /* XXXbe minor(?) incompatibility: iterate in reverse definition order */
    if (!sprop) {
        sprop = SCOPE_LAST_PROP(scope);
    } else {
        while ((sprop = sprop->parent) != NULL) {
            if (!SCOPE_HAD_MIDDLE_DELETE(scope))
                break;
            if (SCOPE_HAS_PROPERTY(scope, sprop))
                break;
        }
    }
    *iteratorp = sprop;
    return sprop;
}

JS_PUBLIC_API(JSBool)
JS_GetPropertyDesc(JSContext *cx, JSObject *obj, JSScopeProperty *sprop,
                   JSPropertyDesc *pd)
{
    JSPropertyOp getter;
    JSScope *scope;
    JSScopeProperty *aprop;
    jsval lastException;
    JSBool wasThrowing;

    pd->id = ID_TO_VALUE(sprop->id);

    wasThrowing = cx->throwing;
    if (wasThrowing) {
        lastException = cx->exception;
        if (JSVAL_IS_GCTHING(lastException) &&
            !js_AddRoot(cx, &lastException, "lastException")) {
                return JS_FALSE;
        }
        cx->throwing = JS_FALSE;
    }

    if (!js_GetProperty(cx, obj, sprop->id, &pd->value)) {
        if (!cx->throwing) {
            pd->flags = JSPD_ERROR;
            pd->value = JSVAL_VOID;
        } else {
            pd->flags = JSPD_EXCEPTION;
            pd->value = cx->exception;
        }
    } else {
        pd->flags = 0;
    }

    cx->throwing = wasThrowing;
    if (wasThrowing) {
        cx->exception = lastException;
        if (JSVAL_IS_GCTHING(lastException))
            js_RemoveRoot(cx->runtime, &lastException);
    }

    getter = sprop->getter;
    pd->flags |= ((sprop->attrs & JSPROP_ENUMERATE) ? JSPD_ENUMERATE : 0)
              | ((sprop->attrs & JSPROP_READONLY)  ? JSPD_READONLY  : 0)
              | ((sprop->attrs & JSPROP_PERMANENT) ? JSPD_PERMANENT : 0)
              | ((getter == js_GetCallVariable)    ? JSPD_VARIABLE  : 0)
              | ((getter == js_GetArgument)        ? JSPD_ARGUMENT  : 0)
              | ((getter == js_GetLocalVariable)   ? JSPD_VARIABLE  : 0);

    /* for Call Object 'real' getter isn't passed in to us */
    if (OBJ_GET_CLASS(cx, obj) == &js_CallClass &&
        getter == js_CallClass.getProperty) {
        /*
         * Property of a heavyweight function's variable object having the
         * class-default getter.  It's either an argument if permanent, or a
         * nested function if impermanent.  Local variables have a special
         * getter (js_GetCallVariable, tested above) and setter, and not the
         * class default.
         */
        pd->flags |= (sprop->attrs & JSPROP_PERMANENT)
                     ? JSPD_ARGUMENT
                     : JSPD_VARIABLE;
    }

    pd->spare = 0;
    pd->slot = (pd->flags & (JSPD_ARGUMENT | JSPD_VARIABLE))
               ? sprop->shortid
               : 0;
    pd->alias = JSVAL_VOID;
    scope = OBJ_SCOPE(obj);
    if (SPROP_HAS_VALID_SLOT(sprop, scope)) {
        for (aprop = SCOPE_LAST_PROP(scope); aprop; aprop = aprop->parent) {
            if (aprop != sprop && aprop->slot == sprop->slot) {
                pd->alias = ID_TO_VALUE(aprop->id);
                break;
            }
        }
    }
    return JS_TRUE;
}

JS_PUBLIC_API(JSBool)
JS_GetPropertyDescArray(JSContext *cx, JSObject *obj, JSPropertyDescArray *pda)
{
    JSClass *clasp;
    JSScope *scope;
    uint32 i, n;
    JSPropertyDesc *pd;
    JSScopeProperty *sprop;

    clasp = OBJ_GET_CLASS(cx, obj);
    if (!OBJ_IS_NATIVE(obj) || (clasp->flags & JSCLASS_NEW_ENUMERATE)) {
        JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL,
                             JSMSG_CANT_DESCRIBE_PROPS, clasp->name);
        return JS_FALSE;
    }
    if (!clasp->enumerate(cx, obj))
        return JS_FALSE;

    /* have no props, or object's scope has not mutated from that of proto */
    scope = OBJ_SCOPE(obj);
    if (scope->object != obj || scope->entryCount == 0) {
        pda->length = 0;
        pda->array = NULL;
        return JS_TRUE;
    }

    n = scope->entryCount;
    if (n > scope->map.nslots)
        n = scope->map.nslots;
    pd = (JSPropertyDesc *) JS_malloc(cx, (size_t)n * sizeof(JSPropertyDesc));
    if (!pd)
        return JS_FALSE;
    i = 0;
    for (sprop = SCOPE_LAST_PROP(scope); sprop; sprop = sprop->parent) {
        if (SCOPE_HAD_MIDDLE_DELETE(scope) && !SCOPE_HAS_PROPERTY(scope, sprop))
            continue;
        if (!js_AddRoot(cx, &pd[i].id, NULL))
            goto bad;
        if (!js_AddRoot(cx, &pd[i].value, NULL))
            goto bad;
        if (!JS_GetPropertyDesc(cx, obj, sprop, &pd[i]))
            goto bad;
        if ((pd[i].flags & JSPD_ALIAS) && !js_AddRoot(cx, &pd[i].alias, NULL))
            goto bad;
        if (++i == n)
            break;
    }
    pda->length = i;
    pda->array = pd;
    return JS_TRUE;

bad:
    pda->length = i + 1;
    pda->array = pd;
    JS_PutPropertyDescArray(cx, pda);
    return JS_FALSE;
}

JS_PUBLIC_API(void)
JS_PutPropertyDescArray(JSContext *cx, JSPropertyDescArray *pda)
{
    JSPropertyDesc *pd;
    uint32 i;

    pd = pda->array;
    for (i = 0; i < pda->length; i++) {
        js_RemoveRoot(cx->runtime, &pd[i].id);
        js_RemoveRoot(cx->runtime, &pd[i].value);
        if (pd[i].flags & JSPD_ALIAS)
            js_RemoveRoot(cx->runtime, &pd[i].alias);
    }
    JS_free(cx, pd);
}

/************************************************************************/

JS_PUBLIC_API(JSBool)
JS_SetDebuggerHandler(JSRuntime *rt, JSTrapHandler handler, void *closure)
{
    rt->debuggerHandler = handler;
    rt->debuggerHandlerData = closure;
    return JS_TRUE;
}

JS_PUBLIC_API(JSBool)
JS_SetSourceHandler(JSRuntime *rt, JSSourceHandler handler, void *closure)
{
    rt->sourceHandler = handler;
    rt->sourceHandlerData = closure;
    return JS_TRUE;
}

JS_PUBLIC_API(JSBool)
JS_SetExecuteHook(JSRuntime *rt, JSInterpreterHook hook, void *closure)
{
    rt->executeHook = hook;
    rt->executeHookData = closure;
    return JS_TRUE;
}

JS_PUBLIC_API(JSBool)
JS_SetCallHook(JSRuntime *rt, JSInterpreterHook hook, void *closure)
{
    rt->callHook = hook;
    rt->callHookData = closure;
    return JS_TRUE;
}

JS_PUBLIC_API(JSBool)
JS_SetObjectHook(JSRuntime *rt, JSObjectHook hook, void *closure)
{
    rt->objectHook = hook;
    rt->objectHookData = closure;
    return JS_TRUE;
}

JS_PUBLIC_API(JSBool)
JS_SetThrowHook(JSRuntime *rt, JSTrapHandler hook, void *closure)
{
    rt->throwHook = hook;
    rt->throwHookData = closure;
    return JS_TRUE;
}

JS_PUBLIC_API(JSBool)
JS_SetDebugErrorHook(JSRuntime *rt, JSDebugErrorHook hook, void *closure)
{
    rt->debugErrorHook = hook;
    rt->debugErrorHookData = closure;
    return JS_TRUE;
}

/************************************************************************/

JS_PUBLIC_API(size_t)
JS_GetObjectTotalSize(JSContext *cx, JSObject *obj)
{
    size_t nbytes;
    JSScope *scope;

    nbytes = sizeof *obj + obj->map->nslots * sizeof obj->slots[0];
    if (OBJ_IS_NATIVE(obj)) {
        scope = OBJ_SCOPE(obj);
        if (scope->object == obj) {
            nbytes += sizeof *scope;
            nbytes += SCOPE_CAPACITY(scope) * sizeof(JSScopeProperty *);
        }
    }
    return nbytes;
}

static size_t
GetAtomTotalSize(JSContext *cx, JSAtom *atom)
{
    size_t nbytes;

    nbytes = sizeof *atom;
    if (ATOM_IS_STRING(atom)) {
        nbytes += sizeof(JSString);
        nbytes += (ATOM_TO_STRING(atom)->length + 1) * sizeof(jschar);
    } else if (ATOM_IS_DOUBLE(atom)) {
        nbytes += sizeof(jsdouble);
    } else if (ATOM_IS_OBJECT(atom)) {
        nbytes += JS_GetObjectTotalSize(cx, ATOM_TO_OBJECT(atom));
    }
    return nbytes;
}

JS_PUBLIC_API(size_t)
JS_GetFunctionTotalSize(JSContext *cx, JSFunction *fun)
{
    size_t nbytes;

    nbytes = sizeof *fun;
    if (fun->object)
        nbytes += JS_GetObjectTotalSize(cx, fun->object);
    if (FUN_INTERPRETED(fun))
        nbytes += JS_GetScriptTotalSize(cx, fun->u.i.script);
    if (fun->atom)
        nbytes += GetAtomTotalSize(cx, fun->atom);
    return nbytes;
}

#include "jsemit.h"

JS_PUBLIC_API(size_t)
JS_GetScriptTotalSize(JSContext *cx, JSScript *script)
{
    size_t nbytes, pbytes;
    JSObject *obj;
    jsatomid i;
    jssrcnote *sn, *notes;
    JSTryNote *tn, *tnotes;
    JSPrincipals *principals;

    nbytes = sizeof *script;
    obj = script->object;
    if (obj)
        nbytes += JS_GetObjectTotalSize(cx, obj);

    nbytes += script->length * sizeof script->code[0];
    nbytes += script->atomMap.length * sizeof script->atomMap.vector[0];
    for (i = 0; i < script->atomMap.length; i++)
        nbytes += GetAtomTotalSize(cx, script->atomMap.vector[i]);

    if (script->filename)
        nbytes += strlen(script->filename) + 1;

    notes = SCRIPT_NOTES(script);
    for (sn = notes; !SN_IS_TERMINATOR(sn); sn = SN_NEXT(sn))
        continue;
    nbytes += (sn - notes + 1) * sizeof *sn;

    tnotes = script->trynotes;
    if (tnotes) {
        for (tn = tnotes; tn->catchStart; tn++)
            continue;
        nbytes += (tn - tnotes + 1) * sizeof *tn;
    }

    principals = script->principals;
    if (principals) {
        JS_ASSERT(principals->refcount);
        pbytes = sizeof *principals;
        if (principals->refcount > 1)
            pbytes = JS_HOWMANY(pbytes, principals->refcount);
        nbytes += pbytes;
    }

    return nbytes;
}

JS_PUBLIC_API(uint32)
JS_GetTopScriptFilenameFlags(JSContext *cx, JSStackFrame *fp)
{
    if (!fp)
        fp = cx->fp;
    while (fp) {
        if (fp->script) {
            return JS_GetScriptFilenameFlags(fp->script);
        }
        fp = fp->down;
    }
    return 0;
 }

JS_PUBLIC_API(uint32)
JS_GetScriptFilenameFlags(JSScript *script)
{
    JS_ASSERT(script);
    if (!script->filename)
        return JSFILENAME_NULL;
    return js_GetScriptFilenameFlags(script->filename);
}

JS_PUBLIC_API(JSBool)
JS_FlagScriptFilenamePrefix(JSRuntime *rt, const char *prefix, uint32 flags)
{
    if (!js_SaveScriptFilenameRT(rt, prefix, flags))
        return JS_FALSE;
    return JS_TRUE;
}

JS_PUBLIC_API(JSBool)
JS_IsSystemObject(JSContext *cx, JSObject *obj)
{
    return (*js_GetGCThingFlags(obj) & GCF_SYSTEM) != 0;
}

JS_PUBLIC_API(void)
JS_FlagSystemObject(JSContext *cx, JSObject *obj)
{
    uint8 *flagp;

    flagp = js_GetGCThingFlags(obj);
    *flagp |= GCF_SYSTEM;
}
