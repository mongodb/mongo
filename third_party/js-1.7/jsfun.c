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
 * JS function support.
 */
#include "jsstddef.h"
#include <string.h>
#include "jstypes.h"
#include "jsbit.h"
#include "jsutil.h" /* Added by JSIFY */
#include "jsapi.h"
#include "jsarray.h"
#include "jsatom.h"
#include "jscntxt.h"
#include "jsconfig.h"
#include "jsdbgapi.h"
#include "jsfun.h"
#include "jsgc.h"
#include "jsinterp.h"
#include "jslock.h"
#include "jsnum.h"
#include "jsobj.h"
#include "jsopcode.h"
#include "jsparse.h"
#include "jsscan.h"
#include "jsscope.h"
#include "jsscript.h"
#include "jsstr.h"
#include "jsexn.h"

#if JS_HAS_GENERATORS
# include "jsiter.h"
#endif

/* Generic function/call/arguments tinyids -- also reflected bit numbers. */
enum {
    CALL_ARGUMENTS  = -1,       /* predefined arguments local variable */
    CALL_CALLEE     = -2,       /* reference to active function's object */
    ARGS_LENGTH     = -3,       /* number of actual args, arity if inactive */
    ARGS_CALLEE     = -4,       /* reference from arguments to active funobj */
    FUN_ARITY       = -5,       /* number of formal parameters; desired argc */
    FUN_NAME        = -6,       /* function name, "" if anonymous */
    FUN_CALLER      = -7        /* Function.prototype.caller, backward compat */
};

#if JSFRAME_OVERRIDE_BITS < 8
# error "not enough override bits in JSStackFrame.flags!"
#endif

#define TEST_OVERRIDE_BIT(fp, tinyid) \
    ((fp)->flags & JS_BIT(JSFRAME_OVERRIDE_SHIFT - ((tinyid) + 1)))

#define SET_OVERRIDE_BIT(fp, tinyid) \
    ((fp)->flags |= JS_BIT(JSFRAME_OVERRIDE_SHIFT - ((tinyid) + 1)))

JSBool
js_GetArgsValue(JSContext *cx, JSStackFrame *fp, jsval *vp)
{
    JSObject *argsobj;

    if (TEST_OVERRIDE_BIT(fp, CALL_ARGUMENTS)) {
        JS_ASSERT(fp->callobj);
        return OBJ_GET_PROPERTY(cx, fp->callobj,
                                ATOM_TO_JSID(cx->runtime->atomState
                                             .argumentsAtom),
                                vp);
    }
    argsobj = js_GetArgsObject(cx, fp);
    if (!argsobj)
        return JS_FALSE;
    *vp = OBJECT_TO_JSVAL(argsobj);
    return JS_TRUE;
}

static JSBool
MarkArgDeleted(JSContext *cx, JSStackFrame *fp, uintN slot)
{
    JSObject *argsobj;
    jsval bmapval, bmapint;
    size_t nbits, nbytes;
    jsbitmap *bitmap;

    argsobj = fp->argsobj;
    (void) JS_GetReservedSlot(cx, argsobj, 0, &bmapval);
    nbits = fp->argc;
    JS_ASSERT(slot < nbits);
    if (JSVAL_IS_VOID(bmapval)) {
        if (nbits <= JSVAL_INT_BITS) {
            bmapint = 0;
            bitmap = (jsbitmap *) &bmapint;
        } else {
            nbytes = JS_HOWMANY(nbits, JS_BITS_PER_WORD) * sizeof(jsbitmap);
            bitmap = (jsbitmap *) JS_malloc(cx, nbytes);
            if (!bitmap)
                return JS_FALSE;
            memset(bitmap, 0, nbytes);
            bmapval = PRIVATE_TO_JSVAL(bitmap);
            JS_SetReservedSlot(cx, argsobj, 0, bmapval);
        }
    } else {
        if (nbits <= JSVAL_INT_BITS) {
            bmapint = JSVAL_TO_INT(bmapval);
            bitmap = (jsbitmap *) &bmapint;
        } else {
            bitmap = (jsbitmap *) JSVAL_TO_PRIVATE(bmapval);
        }
    }
    JS_SET_BIT(bitmap, slot);
    if (bitmap == (jsbitmap *) &bmapint) {
        bmapval = INT_TO_JSVAL(bmapint);
        JS_SetReservedSlot(cx, argsobj, 0, bmapval);
    }
    return JS_TRUE;
}

/* NB: Infallible predicate, false does not mean error/exception. */
static JSBool
ArgWasDeleted(JSContext *cx, JSStackFrame *fp, uintN slot)
{
    JSObject *argsobj;
    jsval bmapval, bmapint;
    jsbitmap *bitmap;

    argsobj = fp->argsobj;
    (void) JS_GetReservedSlot(cx, argsobj, 0, &bmapval);
    if (JSVAL_IS_VOID(bmapval))
        return JS_FALSE;
    if (fp->argc <= JSVAL_INT_BITS) {
        bmapint = JSVAL_TO_INT(bmapval);
        bitmap = (jsbitmap *) &bmapint;
    } else {
        bitmap = (jsbitmap *) JSVAL_TO_PRIVATE(bmapval);
    }
    return JS_TEST_BIT(bitmap, slot) != 0;
}

JSBool
js_GetArgsProperty(JSContext *cx, JSStackFrame *fp, jsid id,
                   JSObject **objp, jsval *vp)
{
    jsval val;
    JSObject *obj;
    uintN slot;

    if (TEST_OVERRIDE_BIT(fp, CALL_ARGUMENTS)) {
        JS_ASSERT(fp->callobj);
        if (!OBJ_GET_PROPERTY(cx, fp->callobj,
                              ATOM_TO_JSID(cx->runtime->atomState
                                           .argumentsAtom),
                              &val)) {
            return JS_FALSE;
        }
        if (JSVAL_IS_PRIMITIVE(val)) {
            obj = js_ValueToNonNullObject(cx, val);
            if (!obj)
                return JS_FALSE;
        } else {
            obj = JSVAL_TO_OBJECT(val);
        }
        *objp = obj;
        return OBJ_GET_PROPERTY(cx, obj, id, vp);
    }

    *objp = NULL;
    *vp = JSVAL_VOID;
    if (JSID_IS_INT(id)) {
        slot = (uintN) JSID_TO_INT(id);
        if (slot < fp->argc) {
            if (fp->argsobj && ArgWasDeleted(cx, fp, slot))
                return OBJ_GET_PROPERTY(cx, fp->argsobj, id, vp);
            *vp = fp->argv[slot];
        } else {
            /*
             * Per ECMA-262 Ed. 3, 10.1.8, last bulleted item, do not share
             * storage between the formal parameter and arguments[k] for all
             * k >= fp->argc && k < fp->fun->nargs.  For example, in
             *
             *   function f(x) { x = 42; return arguments[0]; }
             *   f();
             *
             * the call to f should return undefined, not 42.  If fp->argsobj
             * is null at this point, as it would be in the example, return
             * undefined in *vp.
             */
            if (fp->argsobj)
                return OBJ_GET_PROPERTY(cx, fp->argsobj, id, vp);
        }
    } else {
        if (id == ATOM_TO_JSID(cx->runtime->atomState.lengthAtom)) {
            if (fp->argsobj && TEST_OVERRIDE_BIT(fp, ARGS_LENGTH))
                return OBJ_GET_PROPERTY(cx, fp->argsobj, id, vp);
            *vp = INT_TO_JSVAL((jsint) fp->argc);
        }
    }
    return JS_TRUE;
}

JSObject *
js_GetArgsObject(JSContext *cx, JSStackFrame *fp)
{
    JSObject *argsobj, *global, *parent;

    /*
     * We must be in a function activation; the function must be lightweight
     * or else fp must have a variable object.
     */
    JS_ASSERT(fp->fun && (!(fp->fun->flags & JSFUN_HEAVYWEIGHT) || fp->varobj));

    /* Skip eval and debugger frames. */
    while (fp->flags & JSFRAME_SPECIAL)
        fp = fp->down;

    /* Create an arguments object for fp only if it lacks one. */
    argsobj = fp->argsobj;
    if (argsobj)
        return argsobj;

    /* Link the new object to fp so it can get actual argument values. */
    argsobj = js_NewObject(cx, &js_ArgumentsClass, NULL, NULL);
    if (!argsobj || !JS_SetPrivate(cx, argsobj, fp)) {
        cx->weakRoots.newborn[GCX_OBJECT] = NULL;
        return NULL;
    }

    /*
     * Give arguments an intrinsic scope chain link to fp's global object.
     * Since the arguments object lacks a prototype because js_ArgumentsClass
     * is not initialized, js_NewObject won't assign a default parent to it.
     *
     * Therefore if arguments is used as the head of an eval scope chain (via
     * a direct or indirect call to eval(program, arguments)), any reference
     * to a standard class object in the program will fail to resolve due to
     * js_GetClassPrototype not being able to find a global object containing
     * the standard prototype by starting from arguments and following parent.
     */
    global = fp->scopeChain;
    while ((parent = OBJ_GET_PARENT(cx, global)) != NULL)
        global = parent;
    argsobj->slots[JSSLOT_PARENT] = OBJECT_TO_JSVAL(global);
    fp->argsobj = argsobj;
    return argsobj;
}

static JSBool
args_enumerate(JSContext *cx, JSObject *obj);

JSBool
js_PutArgsObject(JSContext *cx, JSStackFrame *fp)
{
    JSObject *argsobj;
    jsval bmapval, rval;
    JSBool ok;
    JSRuntime *rt;

    /*
     * Reuse args_enumerate here to reflect fp's actual arguments as indexed
     * elements of argsobj.  Do this first, before clearing and freeing the
     * deleted argument slot bitmap, because args_enumerate depends on that.
     */
    argsobj = fp->argsobj;
    ok = args_enumerate(cx, argsobj);

    /*
     * Now clear the deleted argument number bitmap slot and free the bitmap,
     * if one was actually created due to 'delete arguments[0]' or similar.
     */
    (void) JS_GetReservedSlot(cx, argsobj, 0, &bmapval);
    if (!JSVAL_IS_VOID(bmapval)) {
        JS_SetReservedSlot(cx, argsobj, 0, JSVAL_VOID);
        if (fp->argc > JSVAL_INT_BITS)
            JS_free(cx, JSVAL_TO_PRIVATE(bmapval));
    }

    /*
     * Now get the prototype properties so we snapshot fp->fun and fp->argc
     * before fp goes away.
     */
    rt = cx->runtime;
    ok &= js_GetProperty(cx, argsobj, ATOM_TO_JSID(rt->atomState.calleeAtom),
                         &rval);
    ok &= js_SetProperty(cx, argsobj, ATOM_TO_JSID(rt->atomState.calleeAtom),
                         &rval);
    ok &= js_GetProperty(cx, argsobj, ATOM_TO_JSID(rt->atomState.lengthAtom),
                         &rval);
    ok &= js_SetProperty(cx, argsobj, ATOM_TO_JSID(rt->atomState.lengthAtom),
                         &rval);

    /*
     * Clear the private pointer to fp, which is about to go away (js_Invoke).
     * Do this last because the args_enumerate and js_GetProperty calls above
     * need to follow the private slot to find fp.
     */
    ok &= JS_SetPrivate(cx, argsobj, NULL);
    fp->argsobj = NULL;
    return ok;
}

static JSBool
args_delProperty(JSContext *cx, JSObject *obj, jsval id, jsval *vp)
{
    jsint slot;
    JSStackFrame *fp;

    if (!JSVAL_IS_INT(id))
        return JS_TRUE;
    fp = (JSStackFrame *)
         JS_GetInstancePrivate(cx, obj, &js_ArgumentsClass, NULL);
    if (!fp)
        return JS_TRUE;
    JS_ASSERT(fp->argsobj);

    slot = JSVAL_TO_INT(id);
    switch (slot) {
      case ARGS_CALLEE:
      case ARGS_LENGTH:
        SET_OVERRIDE_BIT(fp, slot);
        break;

      default:
        if ((uintN)slot < fp->argc && !MarkArgDeleted(cx, fp, slot))
            return JS_FALSE;
        break;
    }
    return JS_TRUE;
}

static JSBool
args_getProperty(JSContext *cx, JSObject *obj, jsval id, jsval *vp)
{
    jsint slot;
    JSStackFrame *fp;

    if (!JSVAL_IS_INT(id))
        return JS_TRUE;
    fp = (JSStackFrame *)
         JS_GetInstancePrivate(cx, obj, &js_ArgumentsClass, NULL);
    if (!fp)
        return JS_TRUE;
    JS_ASSERT(fp->argsobj);

    slot = JSVAL_TO_INT(id);
    switch (slot) {
      case ARGS_CALLEE:
        if (!TEST_OVERRIDE_BIT(fp, slot))
            *vp = fp->argv ? fp->argv[-2] : OBJECT_TO_JSVAL(fp->fun->object);
        break;

      case ARGS_LENGTH:
        if (!TEST_OVERRIDE_BIT(fp, slot))
            *vp = INT_TO_JSVAL((jsint)fp->argc);
        break;

      default:
        if ((uintN)slot < fp->argc && !ArgWasDeleted(cx, fp, slot))
            *vp = fp->argv[slot];
        break;
    }
    return JS_TRUE;
}

static JSBool
args_setProperty(JSContext *cx, JSObject *obj, jsval id, jsval *vp)
{
    JSStackFrame *fp;
    jsint slot;

    if (!JSVAL_IS_INT(id))
        return JS_TRUE;
    fp = (JSStackFrame *)
         JS_GetInstancePrivate(cx, obj, &js_ArgumentsClass, NULL);
    if (!fp)
        return JS_TRUE;
    JS_ASSERT(fp->argsobj);

    slot = JSVAL_TO_INT(id);
    switch (slot) {
      case ARGS_CALLEE:
      case ARGS_LENGTH:
        SET_OVERRIDE_BIT(fp, slot);
        break;

      default:
        if (FUN_INTERPRETED(fp->fun) &&
            (uintN)slot < fp->argc &&
            !ArgWasDeleted(cx, fp, slot)) {
            fp->argv[slot] = *vp;
        }
        break;
    }
    return JS_TRUE;
}

static JSBool
args_resolve(JSContext *cx, JSObject *obj, jsval id, uintN flags,
             JSObject **objp)
{
    JSStackFrame *fp;
    uintN slot;
    JSString *str;
    JSAtom *atom;
    intN tinyid;
    jsval value;

    *objp = NULL;
    fp = (JSStackFrame *)
         JS_GetInstancePrivate(cx, obj, &js_ArgumentsClass, NULL);
    if (!fp)
        return JS_TRUE;
    JS_ASSERT(fp->argsobj);

    if (JSVAL_IS_INT(id)) {
        slot = JSVAL_TO_INT(id);
        if (slot < fp->argc && !ArgWasDeleted(cx, fp, slot)) {
            /* XXX ECMA specs DontEnum, contrary to other array-like objects */
            if (!js_DefineProperty(cx, obj, INT_JSVAL_TO_JSID(id),
                                   fp->argv[slot],
                                   args_getProperty, args_setProperty,
                                   JS_VERSION_IS_ECMA(cx)
                                   ? 0
                                   : JSPROP_ENUMERATE,
                                   NULL)) {
                return JS_FALSE;
            }
            *objp = obj;
        }
    } else {
        str = JSVAL_TO_STRING(id);
        atom = cx->runtime->atomState.lengthAtom;
        if (str == ATOM_TO_STRING(atom)) {
            tinyid = ARGS_LENGTH;
            value = INT_TO_JSVAL(fp->argc);
        } else {
            atom = cx->runtime->atomState.calleeAtom;
            if (str == ATOM_TO_STRING(atom)) {
                tinyid = ARGS_CALLEE;
                value = fp->argv ? fp->argv[-2]
                                 : OBJECT_TO_JSVAL(fp->fun->object);
            } else {
                atom = NULL;

                /* Quell GCC overwarnings. */
                tinyid = 0;
                value = JSVAL_NULL;
            }
        }

        if (atom && !TEST_OVERRIDE_BIT(fp, tinyid)) {
            if (!js_DefineNativeProperty(cx, obj, ATOM_TO_JSID(atom), value,
                                         args_getProperty, args_setProperty, 0,
                                         SPROP_HAS_SHORTID, tinyid, NULL)) {
                return JS_FALSE;
            }
            *objp = obj;
        }
    }

    return JS_TRUE;
}

static JSBool
args_enumerate(JSContext *cx, JSObject *obj)
{
    JSStackFrame *fp;
    JSObject *pobj;
    JSProperty *prop;
    uintN slot, argc;

    fp = (JSStackFrame *)
         JS_GetInstancePrivate(cx, obj, &js_ArgumentsClass, NULL);
    if (!fp)
        return JS_TRUE;
    JS_ASSERT(fp->argsobj);

    /*
     * Trigger reflection with value snapshot in args_resolve using a series
     * of js_LookupProperty calls.  We handle length, callee, and the indexed
     * argument properties.  We know that args_resolve covers all these cases
     * and creates direct properties of obj, but that it may fail to resolve
     * length or callee if overridden.
     */
    if (!js_LookupProperty(cx, obj,
                           ATOM_TO_JSID(cx->runtime->atomState.lengthAtom),
                           &pobj, &prop)) {
        return JS_FALSE;
    }
    if (prop)
        OBJ_DROP_PROPERTY(cx, pobj, prop);

    if (!js_LookupProperty(cx, obj,
                           ATOM_TO_JSID(cx->runtime->atomState.calleeAtom),
                           &pobj, &prop)) {
        return JS_FALSE;
    }
    if (prop)
        OBJ_DROP_PROPERTY(cx, pobj, prop);

    argc = fp->argc;
    for (slot = 0; slot < argc; slot++) {
        if (!js_LookupProperty(cx, obj, INT_TO_JSID((jsint)slot), &pobj, &prop))
            return JS_FALSE;
        if (prop)
            OBJ_DROP_PROPERTY(cx, pobj, prop);
    }
    return JS_TRUE;
}

#if JS_HAS_GENERATORS
/*
 * If a generator-iterator's arguments or call object escapes, it needs to
 * mark its generator object.
 */
static uint32
args_or_call_mark(JSContext *cx, JSObject *obj, void *arg)
{
    JSStackFrame *fp;

    fp = JS_GetPrivate(cx, obj);
    if (fp && (fp->flags & JSFRAME_GENERATOR))
        GC_MARK(cx, FRAME_TO_GENERATOR(fp)->obj, "FRAME_TO_GENERATOR(fp)->obj");
    return 0;
}
#else
# define args_or_call_mark NULL
#endif

/*
 * The Arguments class is not initialized via JS_InitClass, and must not be,
 * because its name is "Object".  Per ECMA, that causes instances of it to
 * delegate to the object named by Object.prototype.  It also ensures that
 * arguments.toString() returns "[object Object]".
 *
 * The JSClass functions below collaborate to lazily reflect and synchronize
 * actual argument values, argument count, and callee function object stored
 * in a JSStackFrame with their corresponding property values in the frame's
 * arguments object.
 */
JSClass js_ArgumentsClass = {
    js_Object_str,
    JSCLASS_HAS_PRIVATE | JSCLASS_NEW_RESOLVE | JSCLASS_HAS_RESERVED_SLOTS(1) |
    JSCLASS_HAS_CACHED_PROTO(JSProto_Object),
    JS_PropertyStub,    args_delProperty,
    args_getProperty,   args_setProperty,
    args_enumerate,     (JSResolveOp) args_resolve,
    JS_ConvertStub,     JS_FinalizeStub,
    NULL,               NULL,
    NULL,               NULL,
    NULL,               NULL,
    args_or_call_mark,  NULL
};

JSObject *
js_GetCallObject(JSContext *cx, JSStackFrame *fp, JSObject *parent)
{
    JSObject *callobj, *funobj;

    /* Create a call object for fp only if it lacks one. */
    JS_ASSERT(fp->fun);
    callobj = fp->callobj;
    if (callobj)
        return callobj;
    JS_ASSERT(fp->fun);

    /* The default call parent is its function's parent (static link). */
    if (!parent) {
        funobj = fp->argv ? JSVAL_TO_OBJECT(fp->argv[-2]) : fp->fun->object;
        if (funobj)
            parent = OBJ_GET_PARENT(cx, funobj);
    }

    /* Create the call object and link it to its stack frame. */
    callobj = js_NewObject(cx, &js_CallClass, NULL, parent);
    if (!callobj || !JS_SetPrivate(cx, callobj, fp)) {
        cx->weakRoots.newborn[GCX_OBJECT] = NULL;
        return NULL;
    }
    fp->callobj = callobj;

    /* Make callobj be the scope chain and the variables object. */
    JS_ASSERT(fp->scopeChain == parent);
    fp->scopeChain = callobj;
    fp->varobj = callobj;
    return callobj;
}

static JSBool
call_enumerate(JSContext *cx, JSObject *obj);

JSBool
js_PutCallObject(JSContext *cx, JSStackFrame *fp)
{
    JSObject *callobj;
    JSBool ok;
    jsid argsid;
    jsval aval;

    /*
     * Reuse call_enumerate here to reflect all actual args and vars into the
     * call object from fp.
     */
    callobj = fp->callobj;
    if (!callobj)
        return JS_TRUE;
    ok = call_enumerate(cx, callobj);

    /*
     * Get the arguments object to snapshot fp's actual argument values.
     */
    if (fp->argsobj) {
        argsid = ATOM_TO_JSID(cx->runtime->atomState.argumentsAtom);
        ok &= js_GetProperty(cx, callobj, argsid, &aval);
        ok &= js_SetProperty(cx, callobj, argsid, &aval);
        ok &= js_PutArgsObject(cx, fp);
    }

    /*
     * Clear the private pointer to fp, which is about to go away (js_Invoke).
     * Do this last because the call_enumerate and js_GetProperty calls above
     * need to follow the private slot to find fp.
     */
    ok &= JS_SetPrivate(cx, callobj, NULL);
    fp->callobj = NULL;
    return ok;
}

static JSPropertySpec call_props[] = {
    {js_arguments_str,  CALL_ARGUMENTS, JSPROP_PERMANENT,0,0},
    {"__callee__",      CALL_CALLEE,    0,0,0},
    {0,0,0,0,0}
};

static JSBool
call_getProperty(JSContext *cx, JSObject *obj, jsval id, jsval *vp)
{
    JSStackFrame *fp;
    jsint slot;

    if (!JSVAL_IS_INT(id))
        return JS_TRUE;
    fp = (JSStackFrame *) JS_GetPrivate(cx, obj);
    if (!fp)
        return JS_TRUE;
    JS_ASSERT(fp->fun);

    slot = JSVAL_TO_INT(id);
    switch (slot) {
      case CALL_ARGUMENTS:
        if (!TEST_OVERRIDE_BIT(fp, slot)) {
            JSObject *argsobj = js_GetArgsObject(cx, fp);
            if (!argsobj)
                return JS_FALSE;
            *vp = OBJECT_TO_JSVAL(argsobj);
        }
        break;

      case CALL_CALLEE:
        if (!TEST_OVERRIDE_BIT(fp, slot))
            *vp = fp->argv ? fp->argv[-2] : OBJECT_TO_JSVAL(fp->fun->object);
        break;

      default:
        if ((uintN)slot < JS_MAX(fp->argc, fp->fun->nargs))
            *vp = fp->argv[slot];
        break;
    }
    return JS_TRUE;
}

static JSBool
call_setProperty(JSContext *cx, JSObject *obj, jsval id, jsval *vp)
{
    JSStackFrame *fp;
    jsint slot;

    if (!JSVAL_IS_INT(id))
        return JS_TRUE;
    fp = (JSStackFrame *) JS_GetPrivate(cx, obj);
    if (!fp)
        return JS_TRUE;
    JS_ASSERT(fp->fun);

    slot = JSVAL_TO_INT(id);
    switch (slot) {
      case CALL_ARGUMENTS:
      case CALL_CALLEE:
        SET_OVERRIDE_BIT(fp, slot);
        break;

      default:
        if ((uintN)slot < JS_MAX(fp->argc, fp->fun->nargs))
            fp->argv[slot] = *vp;
        break;
    }
    return JS_TRUE;
}

JSBool
js_GetCallVariable(JSContext *cx, JSObject *obj, jsval id, jsval *vp)
{
    JSStackFrame *fp;

    JS_ASSERT(JSVAL_IS_INT(id));
    fp = (JSStackFrame *) JS_GetPrivate(cx, obj);
    if (fp) {
        /* XXX no jsint slot commoning here to avoid MSVC1.52 crashes */
        if ((uintN)JSVAL_TO_INT(id) < fp->nvars)
            *vp = fp->vars[JSVAL_TO_INT(id)];
    }
    return JS_TRUE;
}

JSBool
js_SetCallVariable(JSContext *cx, JSObject *obj, jsval id, jsval *vp)
{
    JSStackFrame *fp;

    JS_ASSERT(JSVAL_IS_INT(id));
    fp = (JSStackFrame *) JS_GetPrivate(cx, obj);
    if (fp) {
        /* XXX jsint slot is block-local here to avoid MSVC1.52 crashes */
        jsint slot = JSVAL_TO_INT(id);
        if ((uintN)slot < fp->nvars)
            fp->vars[slot] = *vp;
    }
    return JS_TRUE;
}

static JSBool
call_enumerate(JSContext *cx, JSObject *obj)
{
    JSStackFrame *fp;
    JSObject *funobj, *pobj;
    JSScope *scope;
    JSScopeProperty *sprop, *cprop;
    JSPropertyOp getter;
    jsval *vec;
    JSAtom *atom;
    JSProperty *prop;

    fp = (JSStackFrame *) JS_GetPrivate(cx, obj);
    if (!fp)
        return JS_TRUE;

    /*
     * Do not enumerate a cloned function object at fp->argv[-2], it may have
     * gained its own (mutable) scope (e.g., a brutally-shared XUL script sets
     * the clone's prototype property).  We must enumerate the function object
     * that was decorated with parameter and local variable properties by the
     * compiler when the compiler created fp->fun, namely fp->fun->object.
     *
     * Contrast with call_resolve, where we prefer fp->argv[-2], because we'll
     * use js_LookupProperty to find any overridden properties in that object,
     * if it was a mutated clone; and if not, we will search its prototype,
     * fp->fun->object, to find compiler-created params and locals.
     */
    funobj = fp->fun->object;
    if (!funobj)
        return JS_TRUE;

    /*
     * Reflect actual args from fp->argv for formal parameters, and local vars
     * and functions in fp->vars for declared variables and nested-at-top-level
     * local functions.
     */
    scope = OBJ_SCOPE(funobj);
    for (sprop = SCOPE_LAST_PROP(scope); sprop; sprop = sprop->parent) {
        getter = sprop->getter;
        if (getter == js_GetArgument)
            vec = fp->argv;
        else if (getter == js_GetLocalVariable)
            vec = fp->vars;
        else
            continue;

        /* Trigger reflection by looking up the unhidden atom for sprop->id. */
        JS_ASSERT(JSID_IS_ATOM(sprop->id));
        atom = JSID_TO_ATOM(sprop->id);
        JS_ASSERT(atom->flags & ATOM_HIDDEN);
        atom = atom->entry.value;

        if (!js_LookupProperty(cx, obj, ATOM_TO_JSID(atom), &pobj, &prop))
            return JS_FALSE;

        /*
         * If we found the property in a different object, don't try sticking
         * it into wrong slots vector. This can occur because we have a mutable
         * __proto__ slot, and cloned function objects rely on their __proto__
         * to delegate to the object that contains the var and arg properties.
         */
        if (!prop || pobj != obj) {
            if (prop)
                OBJ_DROP_PROPERTY(cx, pobj, prop);
            continue;
        }
        cprop = (JSScopeProperty *)prop;
        LOCKED_OBJ_SET_SLOT(obj, cprop->slot, vec[(uint16) sprop->shortid]);
        OBJ_DROP_PROPERTY(cx, obj, prop);
    }

    return JS_TRUE;
}

static JSBool
call_resolve(JSContext *cx, JSObject *obj, jsval id, uintN flags,
             JSObject **objp)
{
    JSStackFrame *fp;
    JSObject *funobj;
    JSString *str;
    JSAtom *atom;
    JSObject *obj2;
    JSProperty *prop;
    JSScopeProperty *sprop;
    JSPropertyOp getter, setter;
    uintN attrs, slot, nslots, spflags;
    jsval *vp, value;
    intN shortid;

    fp = (JSStackFrame *) JS_GetPrivate(cx, obj);
    if (!fp)
        return JS_TRUE;
    JS_ASSERT(fp->fun);

    if (!JSVAL_IS_STRING(id))
        return JS_TRUE;

    funobj = fp->argv ? JSVAL_TO_OBJECT(fp->argv[-2]) : fp->fun->object;
    if (!funobj)
        return JS_TRUE;
    JS_ASSERT((JSFunction *) JS_GetPrivate(cx, funobj) == fp->fun);

    str = JSVAL_TO_STRING(id);
    atom = js_AtomizeString(cx, str, 0);
    if (!atom)
        return JS_FALSE;
    if (!js_LookupHiddenProperty(cx, funobj, ATOM_TO_JSID(atom), &obj2, &prop))
        return JS_FALSE;

    if (prop) {
        if (!OBJ_IS_NATIVE(obj2)) {
            OBJ_DROP_PROPERTY(cx, obj2, prop);
            return JS_TRUE;
        }

        sprop = (JSScopeProperty *) prop;
        getter = sprop->getter;
        attrs = sprop->attrs & ~JSPROP_SHARED;
        slot = (uintN) sprop->shortid;
        OBJ_DROP_PROPERTY(cx, obj2, prop);

        /* Ensure we found an arg or var property for the same function. */
        if ((sprop->flags & SPROP_IS_HIDDEN) &&
            (obj2 == funobj ||
             (JSFunction *) JS_GetPrivate(cx, obj2) == fp->fun)) {
            if (getter == js_GetArgument) {
                vp = fp->argv;
                nslots = JS_MAX(fp->argc, fp->fun->nargs);
                getter = setter = NULL;
            } else {
                JS_ASSERT(getter == js_GetLocalVariable);
                vp = fp->vars;
                nslots = fp->nvars;
                getter = js_GetCallVariable;
                setter = js_SetCallVariable;
            }
            if (slot < nslots) {
                value = vp[slot];
                spflags = SPROP_HAS_SHORTID;
                shortid = (intN) slot;
            } else {
                value = JSVAL_VOID;
                spflags = 0;
                shortid = 0;
            }
            if (!js_DefineNativeProperty(cx, obj, ATOM_TO_JSID(atom), value,
                                         getter, setter, attrs,
                                         spflags, shortid, NULL)) {
                return JS_FALSE;
            }
            *objp = obj;
        }
    }

    return JS_TRUE;
}

static JSBool
call_convert(JSContext *cx, JSObject *obj, JSType type, jsval *vp)
{
    JSStackFrame *fp;

    if (type == JSTYPE_FUNCTION) {
        fp = (JSStackFrame *) JS_GetPrivate(cx, obj);
        if (fp) {
            JS_ASSERT(fp->fun);
            *vp = fp->argv ? fp->argv[-2] : OBJECT_TO_JSVAL(fp->fun->object);
        }
    }
    return JS_TRUE;
}

JSClass js_CallClass = {
    js_Call_str,
    JSCLASS_HAS_PRIVATE | JSCLASS_NEW_RESOLVE | JSCLASS_IS_ANONYMOUS |
    JSCLASS_HAS_CACHED_PROTO(JSProto_Call),
    JS_PropertyStub,    JS_PropertyStub,
    call_getProperty,   call_setProperty,
    call_enumerate,     (JSResolveOp)call_resolve,
    call_convert,       JS_FinalizeStub,
    NULL,               NULL,
    NULL,               NULL,
    NULL,               NULL,
    args_or_call_mark,  NULL,
};

/*
 * ECMA-262 specifies that length is a property of function object instances,
 * but we can avoid that space cost by delegating to a prototype property that
 * is JSPROP_PERMANENT and JSPROP_SHARED.  Each fun_getProperty call computes
 * a fresh length value based on the arity of the individual function object's
 * private data.
 *
 * The extensions below other than length, i.e., the ones not in ECMA-262,
 * are neither JSPROP_READONLY nor JSPROP_SHARED, because for compatibility
 * with ECMA we must allow a delegating object to override them.
 */
#define LENGTH_PROP_ATTRS (JSPROP_READONLY|JSPROP_PERMANENT|JSPROP_SHARED)

static JSPropertySpec function_props[] = {
    {js_arguments_str, CALL_ARGUMENTS, JSPROP_PERMANENT,  0,0},
    {js_arity_str,     FUN_ARITY,      JSPROP_PERMANENT,  0,0},
    {js_caller_str,    FUN_CALLER,     JSPROP_PERMANENT,  0,0},
    {js_length_str,    ARGS_LENGTH,    LENGTH_PROP_ATTRS, 0,0},
    {js_name_str,      FUN_NAME,       JSPROP_PERMANENT,  0,0},
    {0,0,0,0,0}
};

static JSBool
fun_getProperty(JSContext *cx, JSObject *obj, jsval id, jsval *vp)
{
    jsint slot;
    JSFunction *fun;
    JSStackFrame *fp;

    if (!JSVAL_IS_INT(id))
        return JS_TRUE;
    slot = JSVAL_TO_INT(id);

    /*
     * Loop because getter and setter can be delegated from another class,
     * but loop only for ARGS_LENGTH because we must pretend that f.length
     * is in each function instance f, per ECMA-262, instead of only in the
     * Function.prototype object (we use JSPROP_PERMANENT with JSPROP_SHARED
     * to make it appear so).
     *
     * This code couples tightly to the attributes for the function_props[]
     * initializers above, and to js_SetProperty and js_HasOwnPropertyHelper.
     *
     * It's important to allow delegating objects, even though they inherit
     * this getter (fun_getProperty), to override arguments, arity, caller,
     * and name.  If we didn't return early for slot != ARGS_LENGTH, we would
     * clobber *vp with the native property value, instead of letting script
     * override that value in delegating objects.
     *
     * Note how that clobbering is what simulates JSPROP_READONLY for all of
     * the non-standard properties when the directly addressed object (obj)
     * is a function object (i.e., when this loop does not iterate).
     */
    while (!(fun = (JSFunction *)
                   JS_GetInstancePrivate(cx, obj, &js_FunctionClass, NULL))) {
        if (slot != ARGS_LENGTH)
            return JS_TRUE;
        obj = OBJ_GET_PROTO(cx, obj);
        if (!obj)
            return JS_TRUE;
    }

    /* Find fun's top-most activation record. */
    for (fp = cx->fp; fp && (fp->fun != fun || (fp->flags & JSFRAME_SPECIAL));
         fp = fp->down) {
        continue;
    }

    switch (slot) {
      case CALL_ARGUMENTS:
        /* Warn if strict about f.arguments or equivalent unqualified uses. */
        if (!JS_ReportErrorFlagsAndNumber(cx,
                                          JSREPORT_WARNING | JSREPORT_STRICT,
                                          js_GetErrorMessage, NULL,
                                          JSMSG_DEPRECATED_USAGE,
                                          js_arguments_str)) {
            return JS_FALSE;
        }
        if (fp) {
            if (!js_GetArgsValue(cx, fp, vp))
                return JS_FALSE;
        } else {
            *vp = JSVAL_NULL;
        }
        break;

      case ARGS_LENGTH:
      case FUN_ARITY:
            *vp = INT_TO_JSVAL((jsint)fun->nargs);
        break;

      case FUN_NAME:
        *vp = fun->atom
              ? ATOM_KEY(fun->atom)
              : STRING_TO_JSVAL(cx->runtime->emptyString);
        break;

      case FUN_CALLER:
        while (fp && (fp->flags & JSFRAME_SKIP_CALLER) && fp->down)
            fp = fp->down;
        if (fp && fp->down && fp->down->fun && fp->down->argv)
            *vp = fp->down->argv[-2];
        else
            *vp = JSVAL_NULL;
        if (!JSVAL_IS_PRIMITIVE(*vp) && cx->runtime->checkObjectAccess) {
            id = ATOM_KEY(cx->runtime->atomState.callerAtom);
            if (!cx->runtime->checkObjectAccess(cx, obj, id, JSACC_READ, vp))
                return JS_FALSE;
        }
        break;

      default:
        /* XXX fun[0] and fun.arguments[0] are equivalent. */
        if (fp && fp->fun && (uintN)slot < fp->fun->nargs)
            *vp = fp->argv[slot];
        break;
    }

    return JS_TRUE;
}

static JSBool
fun_enumerate(JSContext *cx, JSObject *obj)
{
    jsid prototypeId;
    JSObject *pobj;
    JSProperty *prop;

    prototypeId = ATOM_TO_JSID(cx->runtime->atomState.classPrototypeAtom);
    if (!OBJ_LOOKUP_PROPERTY(cx, obj, prototypeId, &pobj, &prop))
        return JS_FALSE;
    if (prop)
        OBJ_DROP_PROPERTY(cx, pobj, prop);
    return JS_TRUE;
}

static JSBool
fun_resolve(JSContext *cx, JSObject *obj, jsval id, uintN flags,
            JSObject **objp)
{
    JSFunction *fun;
    JSString *str;
    JSAtom *prototypeAtom;

    /*
     * No need to reflect fun.prototype in 'fun.prototype = ...' or in an
     * unqualified reference to prototype, which the emitter looks up as a
     * hidden atom when attempting to bind to a formal parameter or local
     * variable slot.
     */
    if (flags & (JSRESOLVE_ASSIGNING | JSRESOLVE_HIDDEN))
        return JS_TRUE;

    if (!JSVAL_IS_STRING(id))
        return JS_TRUE;

    /* No valid function object should lack private data, but check anyway. */
    fun = (JSFunction *)JS_GetInstancePrivate(cx, obj, &js_FunctionClass, NULL);
    if (!fun || !fun->object)
        return JS_TRUE;

    /*
     * Ok, check whether id is 'prototype' and bootstrap the function object's
     * prototype property.
     */
    str = JSVAL_TO_STRING(id);
    prototypeAtom = cx->runtime->atomState.classPrototypeAtom;
    if (str == ATOM_TO_STRING(prototypeAtom)) {
        JSObject *proto, *parentProto;
        jsval pval;

        proto = parentProto = NULL;
        if (fun->object != obj && fun->object) {
            /*
             * Clone of a function: make its prototype property value have the
             * same class as the clone-parent's prototype.
             */
            if (!OBJ_GET_PROPERTY(cx, fun->object, ATOM_TO_JSID(prototypeAtom),
                                  &pval)) {
                return JS_FALSE;
            }
            if (!JSVAL_IS_PRIMITIVE(pval)) {
                /*
                 * We are about to allocate a new object, so hack the newborn
                 * root until then to protect pval in case it is figuratively
                 * up in the air, with no strong refs protecting it.
                 */
                cx->weakRoots.newborn[GCX_OBJECT] = JSVAL_TO_GCTHING(pval);
                parentProto = JSVAL_TO_OBJECT(pval);
            }
        }

        /*
         * Beware of the wacky case of a user function named Object -- trying
         * to find a prototype for that will recur back here _ad perniciem_.
         */
        if (!parentProto && fun->atom == CLASS_ATOM(cx, Object))
            return JS_TRUE;

        /*
         * If resolving "prototype" in a clone, clone the parent's prototype.
         * Pass the constructor's (obj's) parent as the prototype parent, to
         * avoid defaulting to parentProto.constructor.__parent__.
         */
        proto = js_NewObject(cx, &js_ObjectClass, parentProto,
                             OBJ_GET_PARENT(cx, obj));
        if (!proto)
            return JS_FALSE;

        /*
         * ECMA (15.3.5.2) says that constructor.prototype is DontDelete for
         * user-defined functions, but DontEnum | ReadOnly | DontDelete for
         * native "system" constructors such as Object or Function.  So lazily
         * set the former here in fun_resolve, but eagerly define the latter
         * in JS_InitClass, with the right attributes.
         */
        if (!js_SetClassPrototype(cx, obj, proto,
                                  JSPROP_ENUMERATE | JSPROP_PERMANENT)) {
            cx->weakRoots.newborn[GCX_OBJECT] = NULL;
            return JS_FALSE;
        }
        *objp = obj;
    }

    return JS_TRUE;
}

static JSBool
fun_convert(JSContext *cx, JSObject *obj, JSType type, jsval *vp)
{
    switch (type) {
      case JSTYPE_FUNCTION:
        *vp = OBJECT_TO_JSVAL(obj);
        return JS_TRUE;
      default:
        return js_TryValueOf(cx, obj, type, vp);
    }
}

static void
fun_finalize(JSContext *cx, JSObject *obj)
{
    JSFunction *fun;
    JSScript *script;

    /* No valid function object should lack private data, but check anyway. */
    fun = (JSFunction *) JS_GetPrivate(cx, obj);
    if (!fun)
        return;
    if (fun->object == obj)
        fun->object = NULL;

    /* Null-check required since the parser sets interpreted very early. */
    if (FUN_INTERPRETED(fun) && fun->u.i.script &&
        js_IsAboutToBeFinalized(cx, fun))
    {
        script = fun->u.i.script;
        fun->u.i.script = NULL;
        js_DestroyScript(cx, script);
    }
}

#if JS_HAS_XDR

#include "jsxdrapi.h"

enum {
    JSXDR_FUNARG = 1,
    JSXDR_FUNVAR = 2,
    JSXDR_FUNCONST = 3
};

/* XXX store parent and proto, if defined */
static JSBool
fun_xdrObject(JSXDRState *xdr, JSObject **objp)
{
    JSContext *cx;
    JSFunction *fun;
    uint32 nullAtom;            /* flag to indicate if fun->atom is NULL */
    JSTempValueRooter tvr;
    uint32 flagsword;           /* originally only flags was JS_XDRUint8'd */
    uint16 extraUnused;         /* variable for no longer used field */
    JSAtom *propAtom;
    JSScopeProperty *sprop;
    uint32 userid;              /* NB: holds a signed int-tagged jsval */
    uintN i, n, dupflag;
    uint32 type;
    JSBool ok;
#ifdef DEBUG
    uintN nvars = 0, nargs = 0;
#endif

    cx = xdr->cx;
    if (xdr->mode == JSXDR_ENCODE) {
        /*
         * No valid function object should lack private data, but fail soft
         * (return true, no error report) in case one does due to API pilot
         * or internal error.
         */
        fun = (JSFunction *) JS_GetPrivate(cx, *objp);
        if (!fun)
            return JS_TRUE;
        if (!FUN_INTERPRETED(fun)) {
            JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL,
                                 JSMSG_NOT_SCRIPTED_FUNCTION,
                                 JS_GetFunctionName(fun));
            return JS_FALSE;
        }
        nullAtom = !fun->atom;
        flagsword = ((uint32)fun->u.i.nregexps << 16) | fun->flags;
        extraUnused = 0;
    } else {
        fun = js_NewFunction(cx, NULL, NULL, 0, 0, NULL, NULL);
        if (!fun)
            return JS_FALSE;
    }

    /* From here on, control flow must flow through label out. */
    JS_PUSH_TEMP_ROOT_OBJECT(cx, fun->object, &tvr);
    ok = JS_TRUE;

    if (!JS_XDRUint32(xdr, &nullAtom))
        goto bad;
    if (!nullAtom && !js_XDRStringAtom(xdr, &fun->atom))
        goto bad;

    if (!JS_XDRUint16(xdr, &fun->nargs) ||
        !JS_XDRUint16(xdr, &extraUnused) ||
        !JS_XDRUint16(xdr, &fun->u.i.nvars) ||
        !JS_XDRUint32(xdr, &flagsword)) {
        goto bad;
    }

    /* Assert that all previous writes of extraUnused were writes of 0. */
    JS_ASSERT(extraUnused == 0);

    /* do arguments and local vars */
    if (fun->object) {
        n = fun->nargs + fun->u.i.nvars;
        if (xdr->mode == JSXDR_ENCODE) {
            JSScope *scope;
            JSScopeProperty **spvec, *auto_spvec[8];
            void *mark;

            if (n <= sizeof auto_spvec / sizeof auto_spvec[0]) {
                spvec = auto_spvec;
                mark = NULL;
            } else {
                mark = JS_ARENA_MARK(&cx->tempPool);
                JS_ARENA_ALLOCATE_CAST(spvec, JSScopeProperty **, &cx->tempPool,
                                       n * sizeof(JSScopeProperty *));
                if (!spvec) {
                    JS_ReportOutOfMemory(cx);
                    goto bad;
                }
            }
            scope = OBJ_SCOPE(fun->object);
            for (sprop = SCOPE_LAST_PROP(scope); sprop;
                 sprop = sprop->parent) {
                if (sprop->getter == js_GetArgument) {
                    JS_ASSERT(nargs++ <= fun->nargs);
                    spvec[sprop->shortid] = sprop;
                } else if (sprop->getter == js_GetLocalVariable) {
                    JS_ASSERT(nvars++ <= fun->u.i.nvars);
                    spvec[fun->nargs + sprop->shortid] = sprop;
                }
            }
            for (i = 0; i < n; i++) {
                sprop = spvec[i];
                JS_ASSERT(sprop->flags & SPROP_HAS_SHORTID);
                type = (i < fun->nargs)
                       ? JSXDR_FUNARG
                       : (sprop->attrs & JSPROP_READONLY)
                       ? JSXDR_FUNCONST
                       : JSXDR_FUNVAR;
                userid = INT_TO_JSVAL(sprop->shortid);
                propAtom = JSID_TO_ATOM(sprop->id);
                if (!JS_XDRUint32(xdr, &type) ||
                    !JS_XDRUint32(xdr, &userid) ||
                    !js_XDRCStringAtom(xdr, &propAtom)) {
                    if (mark)
                        JS_ARENA_RELEASE(&cx->tempPool, mark);
                    goto bad;
                }
            }
            if (mark)
                JS_ARENA_RELEASE(&cx->tempPool, mark);
        } else {
            JSPropertyOp getter, setter;

            for (i = n; i != 0; i--) {
                uintN attrs = JSPROP_PERMANENT;

                if (!JS_XDRUint32(xdr, &type) ||
                    !JS_XDRUint32(xdr, &userid) ||
                    !js_XDRCStringAtom(xdr, &propAtom)) {
                    goto bad;
                }
                JS_ASSERT(type == JSXDR_FUNARG || type == JSXDR_FUNVAR ||
                          type == JSXDR_FUNCONST);
                if (type == JSXDR_FUNARG) {
                    getter = js_GetArgument;
                    setter = js_SetArgument;
                    JS_ASSERT(nargs++ <= fun->nargs);
                } else if (type == JSXDR_FUNVAR || type == JSXDR_FUNCONST) {
                    getter = js_GetLocalVariable;
                    setter = js_SetLocalVariable;
                    if (type == JSXDR_FUNCONST)
                        attrs |= JSPROP_READONLY;
                    JS_ASSERT(nvars++ <= fun->u.i.nvars);
                } else {
                    getter = NULL;
                    setter = NULL;
                }

                /* Flag duplicate argument if atom is bound in fun->object. */
                dupflag = SCOPE_GET_PROPERTY(OBJ_SCOPE(fun->object),
                                             ATOM_TO_JSID(propAtom))
                          ? SPROP_IS_DUPLICATE
                          : 0;

                if (!js_AddHiddenProperty(cx, fun->object,
                                          ATOM_TO_JSID(propAtom),
                                          getter, setter, SPROP_INVALID_SLOT,
                                          attrs | JSPROP_SHARED,
                                          dupflag | SPROP_HAS_SHORTID,
                                          JSVAL_TO_INT(userid))) {
                    goto bad;
                }
            }
        }
    }

    if (!js_XDRScript(xdr, &fun->u.i.script, NULL))
        goto bad;

    if (xdr->mode == JSXDR_DECODE) {
        fun->flags = (uint16) flagsword | JSFUN_INTERPRETED;
        fun->u.i.nregexps = (uint16) (flagsword >> 16);

        *objp = fun->object;
        js_CallNewScriptHook(cx, fun->u.i.script, fun);
    }

out:
    JS_POP_TEMP_ROOT(cx, &tvr);
    return ok;

bad:
    ok = JS_FALSE;
    goto out;
}

#else  /* !JS_HAS_XDR */

#define fun_xdrObject NULL

#endif /* !JS_HAS_XDR */

/*
 * [[HasInstance]] internal method for Function objects: fetch the .prototype
 * property of its 'this' parameter, and walks the prototype chain of v (only
 * if v is an object) returning true if .prototype is found.
 */
static JSBool
fun_hasInstance(JSContext *cx, JSObject *obj, jsval v, JSBool *bp)
{
    jsval pval;
    JSString *str;

    if (!OBJ_GET_PROPERTY(cx, obj,
                          ATOM_TO_JSID(cx->runtime->atomState
                                       .classPrototypeAtom),
                          &pval)) {
        return JS_FALSE;
    }

    if (JSVAL_IS_PRIMITIVE(pval)) {
        /*
         * Throw a runtime error if instanceof is called on a function that
         * has a non-object as its .prototype value.
         */
        str = js_DecompileValueGenerator(cx, -1, OBJECT_TO_JSVAL(obj), NULL);
        if (str) {
            JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL,
                                 JSMSG_BAD_PROTOTYPE, JS_GetStringBytes(str));
        }
        return JS_FALSE;
    }

    return js_IsDelegate(cx, JSVAL_TO_OBJECT(pval), v, bp);
}

static uint32
fun_mark(JSContext *cx, JSObject *obj, void *arg)
{
    JSFunction *fun;

    fun = (JSFunction *) JS_GetPrivate(cx, obj);
    if (fun) {
        GC_MARK(cx, fun, "private");
        if (fun->atom)
            GC_MARK_ATOM(cx, fun->atom);
        if (FUN_INTERPRETED(fun) && fun->u.i.script)
            js_MarkScript(cx, fun->u.i.script);
    }
    return 0;
}

static uint32
fun_reserveSlots(JSContext *cx, JSObject *obj)
{
    JSFunction *fun;

    fun = (JSFunction *) JS_GetPrivate(cx, obj);
    return (fun && FUN_INTERPRETED(fun)) ? fun->u.i.nregexps : 0;
}

/*
 * Reserve two slots in all function objects for XPConnect.  Note that this
 * does not bloat every instance, only those on which reserved slots are set,
 * and those on which ad-hoc properties are defined.
 */
JS_FRIEND_DATA(JSClass) js_FunctionClass = {
    js_Function_str,
    JSCLASS_HAS_PRIVATE | JSCLASS_NEW_RESOLVE | JSCLASS_HAS_RESERVED_SLOTS(2) |
    JSCLASS_HAS_CACHED_PROTO(JSProto_Function),
    JS_PropertyStub,  JS_PropertyStub,
    fun_getProperty,  JS_PropertyStub,
    fun_enumerate,    (JSResolveOp)fun_resolve,
    fun_convert,      fun_finalize,
    NULL,             NULL,
    NULL,             NULL,
    fun_xdrObject,    fun_hasInstance,
    fun_mark,         fun_reserveSlots
};

JSBool
js_fun_toString(JSContext *cx, JSObject *obj, uint32 indent,
                uintN argc, jsval *argv, jsval *rval)
{
    jsval fval;
    JSFunction *fun;
    JSString *str;

    if (!argv) {
        JS_ASSERT(JS_ObjectIsFunction(cx, obj));
    } else {
        fval = argv[-1];
        if (!VALUE_IS_FUNCTION(cx, fval)) {
            /*
             * If we don't have a function to start off with, try converting
             * the object to a function.  If that doesn't work, complain.
             */
            if (JSVAL_IS_OBJECT(fval)) {
                obj = JSVAL_TO_OBJECT(fval);
                if (!OBJ_GET_CLASS(cx, obj)->convert(cx, obj, JSTYPE_FUNCTION,
                                                     &fval)) {
                    return JS_FALSE;
                }
                argv[-1] = fval;
            }
            if (!VALUE_IS_FUNCTION(cx, fval)) {
                JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL,
                                     JSMSG_INCOMPATIBLE_PROTO,
                                     js_Function_str, js_toString_str,
                                     JS_GetTypeName(cx,
                                                    JS_TypeOfValue(cx, fval)));
                return JS_FALSE;
            }
        }

        obj = JSVAL_TO_OBJECT(fval);
    }

    fun = (JSFunction *) JS_GetPrivate(cx, obj);
    if (!fun)
        return JS_TRUE;
    if (argc && !js_ValueToECMAUint32(cx, argv[0], &indent))
        return JS_FALSE;
    str = JS_DecompileFunction(cx, fun, (uintN)indent);
    if (!str)
        return JS_FALSE;
    *rval = STRING_TO_JSVAL(str);
    return JS_TRUE;
}

static JSBool
fun_toString(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval)
{
    return js_fun_toString(cx, obj, 0, argc, argv, rval);
}

#if JS_HAS_TOSOURCE
static JSBool
fun_toSource(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval)
{
    return js_fun_toString(cx, obj, JS_DONT_PRETTY_PRINT, argc, argv, rval);
}
#endif

static const char call_str[] = "call";

static JSBool
fun_call(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval)
{
    jsval fval, *sp, *oldsp;
    JSString *str;
    void *mark;
    uintN i;
    JSStackFrame *fp;
    JSBool ok;

    if (!OBJ_DEFAULT_VALUE(cx, obj, JSTYPE_FUNCTION, &argv[-1]))
        return JS_FALSE;
    fval = argv[-1];

    if (!VALUE_IS_FUNCTION(cx, fval)) {
        str = JS_ValueToString(cx, fval);
        if (str) {
            JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL,
                                 JSMSG_INCOMPATIBLE_PROTO,
                                 js_Function_str, call_str,
                                 JS_GetStringBytes(str));
        }
        return JS_FALSE;
    }

    if (argc == 0) {
        /* Call fun with its global object as the 'this' param if no args. */
        obj = NULL;
    } else {
        /* Otherwise convert the first arg to 'this' and skip over it. */
        if (!js_ValueToObject(cx, argv[0], &obj))
            return JS_FALSE;
        argc--;
        argv++;
    }

    /* Allocate stack space for fval, obj, and the args. */
    sp = js_AllocStack(cx, 2 + argc, &mark);
    if (!sp)
        return JS_FALSE;

    /* Push fval, obj, and the args. */
    *sp++ = fval;
    *sp++ = OBJECT_TO_JSVAL(obj);
    for (i = 0; i < argc; i++)
        *sp++ = argv[i];

    /* Lift current frame to include the args and do the call. */
    fp = cx->fp;
    oldsp = fp->sp;
    fp->sp = sp;
    ok = js_Invoke(cx, argc, JSINVOKE_INTERNAL | JSINVOKE_SKIP_CALLER);

    /* Store rval and pop stack back to our frame's sp. */
    *rval = fp->sp[-1];
    fp->sp = oldsp;
    js_FreeStack(cx, mark);
    return ok;
}

static JSBool
fun_apply(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval)
{
    jsval fval, *sp, *oldsp;
    JSString *str;
    JSObject *aobj;
    jsuint length;
    JSBool arraylike, ok;
    void *mark;
    uintN i;
    JSStackFrame *fp;

    if (argc == 0) {
        /* Will get globalObject as 'this' and no other arguments. */
        return fun_call(cx, obj, argc, argv, rval);
    }

    if (!OBJ_DEFAULT_VALUE(cx, obj, JSTYPE_FUNCTION, &argv[-1]))
        return JS_FALSE;
    fval = argv[-1];

    if (!VALUE_IS_FUNCTION(cx, fval)) {
        str = JS_ValueToString(cx, fval);
        if (str) {
            JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL,
                                 JSMSG_INCOMPATIBLE_PROTO,
                                 js_Function_str, "apply",
                                 JS_GetStringBytes(str));
        }
        return JS_FALSE;
    }

    /* Quell GCC overwarnings. */
    aobj = NULL;
    length = 0;

    if (argc >= 2) {
        /* If the 2nd arg is null or void, call the function with 0 args. */
        if (JSVAL_IS_NULL(argv[1]) || JSVAL_IS_VOID(argv[1])) {
            argc = 0;
        } else {
            /* The second arg must be an array (or arguments object). */
            arraylike = JS_FALSE;
            if (!JSVAL_IS_PRIMITIVE(argv[1])) {
                aobj = JSVAL_TO_OBJECT(argv[1]);
                if (!js_IsArrayLike(cx, aobj, &arraylike, &length))
                    return JS_FALSE;
            }
            if (!arraylike) {
                JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL,
                                     JSMSG_BAD_APPLY_ARGS, "apply");
                return JS_FALSE;
            }
        }
    }

    /* Convert the first arg to 'this' and skip over it. */
    if (!js_ValueToObject(cx, argv[0], &obj))
        return JS_FALSE;

    /* Allocate stack space for fval, obj, and the args. */
    argc = (uintN)JS_MIN(length, ARRAY_INIT_LIMIT - 1);
    sp = js_AllocStack(cx, 2 + argc, &mark);
    if (!sp)
        return JS_FALSE;

    /* Push fval, obj, and aobj's elements as args. */
    *sp++ = fval;
    *sp++ = OBJECT_TO_JSVAL(obj);
    for (i = 0; i < argc; i++) {
        ok = JS_GetElement(cx, aobj, (jsint)i, sp);
        if (!ok)
            goto out;
        sp++;
    }

    /* Lift current frame to include the args and do the call. */
    fp = cx->fp;
    oldsp = fp->sp;
    fp->sp = sp;
    ok = js_Invoke(cx, argc, JSINVOKE_INTERNAL | JSINVOKE_SKIP_CALLER);

    /* Store rval and pop stack back to our frame's sp. */
    *rval = fp->sp[-1];
    fp->sp = oldsp;
out:
    js_FreeStack(cx, mark);
    return ok;
}

#ifdef NARCISSUS
static JSBool
fun_applyConstructor(JSContext *cx, JSObject *obj, uintN argc, jsval *argv,
                     jsval *rval)
{
    JSObject *aobj;
    uintN length, i;
    void *mark;
    jsval *sp, *newsp, *oldsp;
    JSStackFrame *fp;
    JSBool ok;

    if (JSVAL_IS_PRIMITIVE(argv[0]) ||
        (aobj = JSVAL_TO_OBJECT(argv[0]),
         OBJ_GET_CLASS(cx, aobj) != &js_ArrayClass &&
         OBJ_GET_CLASS(cx, aobj) != &js_ArgumentsClass)) {
        JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL,
                             JSMSG_BAD_APPLY_ARGS, "__applyConstruct__");
        return JS_FALSE;
    }

    if (!js_GetLengthProperty(cx, aobj, &length))
        return JS_FALSE;

    if (length >= ARRAY_INIT_LIMIT)
        length = ARRAY_INIT_LIMIT - 1;
    newsp = sp = js_AllocStack(cx, 2 + length, &mark);
    if (!sp)
        return JS_FALSE;

    fp = cx->fp;
    oldsp = fp->sp;
    *sp++ = OBJECT_TO_JSVAL(obj);
    *sp++ = JSVAL_NULL; /* This is filled automagically. */
    for (i = 0; i < length; i++) {
        ok = JS_GetElement(cx, aobj, (jsint)i, sp);
        if (!ok)
            goto out;
        sp++;
    }

    oldsp = fp->sp;
    fp->sp = sp;
    ok = js_InvokeConstructor(cx, newsp, length);

    *rval = fp->sp[-1];
    fp->sp = oldsp;
out:
    js_FreeStack(cx, mark);
    return ok;
}
#endif

static JSFunctionSpec function_methods[] = {
#if JS_HAS_TOSOURCE
    {js_toSource_str,   fun_toSource,   0,0,0},
#endif
    {js_toString_str,   fun_toString,   1,0,0},
    {"apply",           fun_apply,      2,0,0},
    {call_str,          fun_call,       1,0,0},
#ifdef NARCISSUS
    {"__applyConstructor__", fun_applyConstructor, 1,0,0},
#endif
    {0,0,0,0,0}
};

JSBool
js_IsIdentifier(JSString *str)
{
    size_t length;
    jschar c, *chars, *end, *s;

    length = JSSTRING_LENGTH(str);
    if (length == 0)
        return JS_FALSE;
    chars = JSSTRING_CHARS(str);
    c = *chars;
    if (!JS_ISIDSTART(c))
        return JS_FALSE;
    end = chars + length;
    for (s = chars + 1; s != end; ++s) {
        c = *s;
        if (!JS_ISIDENT(c))
            return JS_FALSE;
    }
    return !js_IsKeyword(chars, length);
}

static JSBool
Function(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval)
{
    JSStackFrame *fp, *caller;
    JSFunction *fun;
    JSObject *parent;
    uintN i, n, lineno, dupflag;
    JSAtom *atom;
    const char *filename;
    JSObject *obj2;
    JSProperty *prop;
    JSScopeProperty *sprop;
    JSString *str, *arg;
    void *mark;
    JSTokenStream *ts;
    JSPrincipals *principals;
    jschar *collected_args, *cp;
    size_t arg_length, args_length, old_args_length;
    JSTokenType tt;
    JSBool ok;

    fp = cx->fp;
    if (!(fp->flags & JSFRAME_CONSTRUCTING)) {
        obj = js_NewObject(cx, &js_FunctionClass, NULL, NULL);
        if (!obj)
            return JS_FALSE;
        *rval = OBJECT_TO_JSVAL(obj);
    }
    fun = (JSFunction *) JS_GetPrivate(cx, obj);
    if (fun)
        return JS_TRUE;

    /*
     * NB: (new Function) is not lexically closed by its caller, it's just an
     * anonymous function in the top-level scope that its constructor inhabits.
     * Thus 'var x = 42; f = new Function("return x"); print(f())' prints 42,
     * and so would a call to f from another top-level's script or function.
     *
     * In older versions, before call objects, a new Function was adopted by
     * its running context's globalObject, which might be different from the
     * top-level reachable from scopeChain (in HTML frames, e.g.).
     */
    parent = OBJ_GET_PARENT(cx, JSVAL_TO_OBJECT(argv[-2]));

    fun = js_NewFunction(cx, obj, NULL, 0, JSFUN_LAMBDA, parent,
                         cx->runtime->atomState.anonymousAtom);

    if (!fun)
        return JS_FALSE;

    /*
     * Function is static and not called directly by other functions in this
     * file, therefore it is callable only as a native function by js_Invoke.
     * Find the scripted caller, possibly skipping other native frames such as
     * are built for Function.prototype.call or .apply activations that invoke
     * Function indirectly from a script.
     */
    JS_ASSERT(!fp->script && fp->fun && fp->fun->u.n.native == Function);
    caller = JS_GetScriptedCaller(cx, fp);
    if (caller) {
        filename = caller->script->filename;
        lineno = js_PCToLineNumber(cx, caller->script, caller->pc);
        principals = JS_EvalFramePrincipals(cx, fp, caller);
    } else {
        filename = NULL;
        lineno = 0;
        principals = NULL;
    }

    /* Belt-and-braces: check that the caller has access to parent. */
    if (!js_CheckPrincipalsAccess(cx, parent, principals,
                                  CLASS_ATOM(cx, Function))) {
        return JS_FALSE;
    }

    n = argc ? argc - 1 : 0;
    if (n > 0) {
        /*
         * Collect the function-argument arguments into one string, separated
         * by commas, then make a tokenstream from that string, and scan it to
         * get the arguments.  We need to throw the full scanner at the
         * problem, because the argument string can legitimately contain
         * comments and linefeeds.  XXX It might be better to concatenate
         * everything up into a function definition and pass it to the
         * compiler, but doing it this way is less of a delta from the old
         * code.  See ECMA 15.3.2.1.
         */
        args_length = 0;
        for (i = 0; i < n; i++) {
            /* Collect the lengths for all the function-argument arguments. */
            arg = js_ValueToString(cx, argv[i]);
            if (!arg)
                return JS_FALSE;
            argv[i] = STRING_TO_JSVAL(arg);

            /*
             * Check for overflow.  The < test works because the maximum
             * JSString length fits in 2 fewer bits than size_t has.
             */
            old_args_length = args_length;
            args_length = old_args_length + JSSTRING_LENGTH(arg);
            if (args_length < old_args_length) {
                JS_ReportOutOfMemory(cx);
                return JS_FALSE;
            }
        }

        /* Add 1 for each joining comma and check for overflow (two ways). */
        old_args_length = args_length;
        args_length = old_args_length + n - 1;
        if (args_length < old_args_length ||
            args_length >= ~(size_t)0 / sizeof(jschar)) {
            JS_ReportOutOfMemory(cx);
            return JS_FALSE;
        }

        /*
         * Allocate a string to hold the concatenated arguments, including room
         * for a terminating 0.  Mark cx->tempPool for later release, to free
         * collected_args and its tokenstream in one swoop.
         */
        mark = JS_ARENA_MARK(&cx->tempPool);
        JS_ARENA_ALLOCATE_CAST(cp, jschar *, &cx->tempPool,
                               (args_length+1) * sizeof(jschar));
        if (!cp) {
            JS_ReportOutOfMemory(cx);
            return JS_FALSE;
        }
        collected_args = cp;

        /*
         * Concatenate the arguments into the new string, separated by commas.
         */
        for (i = 0; i < n; i++) {
            arg = JSVAL_TO_STRING(argv[i]);
            arg_length = JSSTRING_LENGTH(arg);
            (void) js_strncpy(cp, JSSTRING_CHARS(arg), arg_length);
            cp += arg_length;

            /* Add separating comma or terminating 0. */
            *cp++ = (i + 1 < n) ? ',' : 0;
        }

        /*
         * Make a tokenstream (allocated from cx->tempPool) that reads from
         * the given string.
         */
        ts = js_NewTokenStream(cx, collected_args, args_length, filename,
                               lineno, principals);
        if (!ts) {
            JS_ARENA_RELEASE(&cx->tempPool, mark);
            return JS_FALSE;
        }

        /* The argument string may be empty or contain no tokens. */
        tt = js_GetToken(cx, ts);
        if (tt != TOK_EOF) {
            for (;;) {
                /*
                 * Check that it's a name.  This also implicitly guards against
                 * TOK_ERROR, which was already reported.
                 */
                if (tt != TOK_NAME)
                    goto bad_formal;

                /*
                 * Get the atom corresponding to the name from the tokenstream;
                 * we're assured at this point that it's a valid identifier.
                 */
                atom = CURRENT_TOKEN(ts).t_atom;
                if (!js_LookupHiddenProperty(cx, obj, ATOM_TO_JSID(atom),
                                             &obj2, &prop)) {
                    goto bad_formal;
                }
                sprop = (JSScopeProperty *) prop;
                dupflag = 0;
                if (sprop) {
                    ok = JS_TRUE;
                    if (obj2 == obj) {
                        const char *name = js_AtomToPrintableString(cx, atom);

                        /*
                         * A duplicate parameter name. We force a duplicate
                         * node on the SCOPE_LAST_PROP(scope) list with the
                         * same id, distinguished by the SPROP_IS_DUPLICATE
                         * flag, and not mapped by an entry in scope.
                         */
                        JS_ASSERT(sprop->getter == js_GetArgument);
                        ok = name &&
                             js_ReportCompileErrorNumber(cx, ts,
                                                         JSREPORT_TS |
                                                         JSREPORT_WARNING |
                                                         JSREPORT_STRICT,
                                                         JSMSG_DUPLICATE_FORMAL,
                                                         name);

                        dupflag = SPROP_IS_DUPLICATE;
                    }
                    OBJ_DROP_PROPERTY(cx, obj2, prop);
                    if (!ok)
                        goto bad_formal;
                    sprop = NULL;
                }
                if (!js_AddHiddenProperty(cx, fun->object, ATOM_TO_JSID(atom),
                                          js_GetArgument, js_SetArgument,
                                          SPROP_INVALID_SLOT,
                                          JSPROP_PERMANENT | JSPROP_SHARED,
                                          dupflag | SPROP_HAS_SHORTID,
                                          fun->nargs)) {
                    goto bad_formal;
                }
                if (fun->nargs == JS_BITMASK(16)) {
                    JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL,
                                         JSMSG_TOO_MANY_FUN_ARGS);
                    goto bad;
                }
                fun->nargs++;

                /*
                 * Get the next token.  Stop on end of stream.  Otherwise
                 * insist on a comma, get another name, and iterate.
                 */
                tt = js_GetToken(cx, ts);
                if (tt == TOK_EOF)
                    break;
                if (tt != TOK_COMMA)
                    goto bad_formal;
                tt = js_GetToken(cx, ts);
            }
        }

        /* Clean up. */
        ok = js_CloseTokenStream(cx, ts);
        JS_ARENA_RELEASE(&cx->tempPool, mark);
        if (!ok)
            return JS_FALSE;
    }

    if (argc) {
        str = js_ValueToString(cx, argv[argc-1]);
    } else {
        /* Can't use cx->runtime->emptyString because we're called too early. */
        str = js_NewStringCopyZ(cx, js_empty_ucstr, 0);
    }
    if (!str)
        return JS_FALSE;
    if (argv) {
        /* Use the last arg (or this if argc == 0) as a local GC root. */
        argv[(intN)(argc-1)] = STRING_TO_JSVAL(str);
    }

    mark = JS_ARENA_MARK(&cx->tempPool);
    ts = js_NewTokenStream(cx, JSSTRING_CHARS(str), JSSTRING_LENGTH(str),
                           filename, lineno, principals);
    if (!ts) {
        ok = JS_FALSE;
    } else {
        ok = js_CompileFunctionBody(cx, ts, fun) &&
             js_CloseTokenStream(cx, ts);
    }
    JS_ARENA_RELEASE(&cx->tempPool, mark);
    return ok;

bad_formal:
    /*
     * Report "malformed formal parameter" iff no illegal char or similar
     * scanner error was already reported.
     */
    if (!(ts->flags & TSF_ERROR))
        JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_BAD_FORMAL);

bad:
    /*
     * Clean up the arguments string and tokenstream if we failed to parse
     * the arguments.
     */
    (void)js_CloseTokenStream(cx, ts);
    JS_ARENA_RELEASE(&cx->tempPool, mark);
    return JS_FALSE;
}

JSObject *
js_InitFunctionClass(JSContext *cx, JSObject *obj)
{
    JSObject *proto;
    JSAtom *atom;
    JSFunction *fun;

    proto = JS_InitClass(cx, obj, NULL, &js_FunctionClass, Function, 1,
                         function_props, function_methods, NULL, NULL);
    if (!proto)
        return NULL;
    atom = js_Atomize(cx, js_FunctionClass.name, strlen(js_FunctionClass.name),
                      0);
    if (!atom)
        goto bad;
    fun = js_NewFunction(cx, proto, NULL, 0, 0, obj, NULL);
    if (!fun)
        goto bad;
    fun->u.i.script = js_NewScript(cx, 1, 0, 0);
    if (!fun->u.i.script)
        goto bad;
    fun->u.i.script->code[0] = JSOP_STOP;
    fun->flags |= JSFUN_INTERPRETED;
    return proto;

bad:
    cx->weakRoots.newborn[GCX_OBJECT] = NULL;
    return NULL;
}

JSObject *
js_InitCallClass(JSContext *cx, JSObject *obj)
{
    JSObject *proto;

    proto = JS_InitClass(cx, obj, NULL, &js_CallClass, NULL, 0,
                         call_props, NULL, NULL, NULL);
    if (!proto)
        return NULL;

    /*
     * Null Call.prototype's proto slot so that Object.prototype.* does not
     * pollute the scope of heavyweight functions.
     */
    OBJ_SET_PROTO(cx, proto, NULL);
    return proto;
}

JSFunction *
js_NewFunction(JSContext *cx, JSObject *funobj, JSNative native, uintN nargs,
               uintN flags, JSObject *parent, JSAtom *atom)
{
    JSFunction *fun;
    JSTempValueRooter tvr;

    /* If funobj is null, allocate an object for it. */
    if (funobj) {
        OBJ_SET_PARENT(cx, funobj, parent);
    } else {
        funobj = js_NewObject(cx, &js_FunctionClass, NULL, parent);
        if (!funobj)
            return NULL;
    }

    /* Protect fun from any potential GC callback. */
    JS_PUSH_SINGLE_TEMP_ROOT(cx, OBJECT_TO_JSVAL(funobj), &tvr);

    /*
     * Allocate fun after allocating funobj so slot allocation in js_NewObject
     * does not wipe out fun from newborn[GCX_PRIVATE].
     */
    fun = (JSFunction *) js_NewGCThing(cx, GCX_PRIVATE, sizeof(JSFunction));
    if (!fun)
        goto out;

    /* Initialize all function members. */
    fun->object = NULL;
    fun->nargs = nargs;
    fun->flags = flags & JSFUN_FLAGS_MASK;
    fun->u.n.native = native;
    fun->u.n.extra = 0;
    fun->u.n.spare = 0;
    fun->atom = atom;
    fun->clasp = NULL;

    /* Link fun to funobj and vice versa. */
    if (!js_LinkFunctionObject(cx, fun, funobj)) {
        cx->weakRoots.newborn[GCX_OBJECT] = NULL;
        fun = NULL;
    }

out:
    JS_POP_TEMP_ROOT(cx, &tvr);
    return fun;
}

JSObject *
js_CloneFunctionObject(JSContext *cx, JSObject *funobj, JSObject *parent)
{
    JSObject *newfunobj;
    JSFunction *fun;

    JS_ASSERT(OBJ_GET_CLASS(cx, funobj) == &js_FunctionClass);
    newfunobj = js_NewObject(cx, &js_FunctionClass, funobj, parent);
    if (!newfunobj)
        return NULL;
    fun = (JSFunction *) JS_GetPrivate(cx, funobj);
    if (!js_LinkFunctionObject(cx, fun, newfunobj)) {
        cx->weakRoots.newborn[GCX_OBJECT] = NULL;
        return NULL;
    }
    return newfunobj;
}

JSBool
js_LinkFunctionObject(JSContext *cx, JSFunction *fun, JSObject *funobj)
{
    if (!fun->object)
        fun->object = funobj;
    return JS_SetPrivate(cx, funobj, fun);
}

JSFunction *
js_DefineFunction(JSContext *cx, JSObject *obj, JSAtom *atom, JSNative native,
                  uintN nargs, uintN attrs)
{
    JSFunction *fun;

    fun = js_NewFunction(cx, NULL, native, nargs, attrs, obj, atom);
    if (!fun)
        return NULL;
    if (!OBJ_DEFINE_PROPERTY(cx, obj, ATOM_TO_JSID(atom),
                             OBJECT_TO_JSVAL(fun->object),
                             NULL, NULL,
                             attrs & ~JSFUN_FLAGS_MASK, NULL)) {
        return NULL;
    }
    return fun;
}

#if (JSV2F_CONSTRUCT & JSV2F_SEARCH_STACK)
# error "JSINVOKE_CONSTRUCT and JSV2F_SEARCH_STACK are not disjoint!"
#endif

JSFunction *
js_ValueToFunction(JSContext *cx, jsval *vp, uintN flags)
{
    jsval v;
    JSObject *obj;

    v = *vp;
    obj = NULL;
    if (JSVAL_IS_OBJECT(v)) {
        obj = JSVAL_TO_OBJECT(v);
        if (obj && OBJ_GET_CLASS(cx, obj) != &js_FunctionClass) {
            if (!OBJ_DEFAULT_VALUE(cx, obj, JSTYPE_FUNCTION, &v))
                return NULL;
            obj = VALUE_IS_FUNCTION(cx, v) ? JSVAL_TO_OBJECT(v) : NULL;
        }
    }
    if (!obj) {
        js_ReportIsNotFunction(cx, vp, flags);
        return NULL;
    }
    return (JSFunction *) JS_GetPrivate(cx, obj);
}

JSObject *
js_ValueToFunctionObject(JSContext *cx, jsval *vp, uintN flags)
{
    JSFunction *fun;
    JSObject *funobj;
    JSStackFrame *caller;
    JSPrincipals *principals;

    if (VALUE_IS_FUNCTION(cx, *vp))
        return JSVAL_TO_OBJECT(*vp);

    fun = js_ValueToFunction(cx, vp, flags);
    if (!fun)
        return NULL;
    funobj = fun->object;
    *vp = OBJECT_TO_JSVAL(funobj);

    caller = JS_GetScriptedCaller(cx, cx->fp);
    if (caller) {
        principals = caller->script->principals;
    } else {
        /* No scripted caller, don't allow access. */
        principals = NULL;
    }

    if (!js_CheckPrincipalsAccess(cx, funobj, principals,
                                  fun->atom
                                  ? fun->atom
                                  : cx->runtime->atomState.anonymousAtom)) {
        return NULL;
    }
    return funobj;
}

JSObject *
js_ValueToCallableObject(JSContext *cx, jsval *vp, uintN flags)
{
    JSObject *callable;

    callable = JSVAL_IS_PRIMITIVE(*vp) ? NULL : JSVAL_TO_OBJECT(*vp);
    if (callable &&
        ((callable->map->ops == &js_ObjectOps)
         ? OBJ_GET_CLASS(cx, callable)->call
         : callable->map->ops->call)) {
        *vp = OBJECT_TO_JSVAL(callable);
    } else {
        callable = js_ValueToFunctionObject(cx, vp, flags);
    }
    return callable;
}

void
js_ReportIsNotFunction(JSContext *cx, jsval *vp, uintN flags)
{
    JSStackFrame *fp;
    JSString *str;
    JSTempValueRooter tvr;
    const char *bytes, *source;

    for (fp = cx->fp; fp && !fp->spbase; fp = fp->down)
        continue;
    str = js_DecompileValueGenerator(cx,
                                     (fp && fp->spbase <= vp && vp < fp->sp)
                                     ? vp - fp->sp
                                     : (flags & JSV2F_SEARCH_STACK)
                                     ? JSDVG_SEARCH_STACK
                                     : JSDVG_IGNORE_STACK,
                                     *vp,
                                     NULL);
    if (str) {
        JS_PUSH_TEMP_ROOT_STRING(cx, str, &tvr);
        bytes = JS_GetStringBytes(str);
        if (flags & JSV2F_ITERATOR) {
            source = js_ValueToPrintableSource(cx, *vp);
            if (source) {
                JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL,
                                     JSMSG_BAD_ITERATOR,
                                     bytes, js_iterator_str, source);
            }
        } else {
            JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL,
                                 (uintN)((flags & JSV2F_CONSTRUCT)
                                         ? JSMSG_NOT_CONSTRUCTOR
                                         : JSMSG_NOT_FUNCTION),
                                 bytes);
        }
        JS_POP_TEMP_ROOT(cx, &tvr);
    }
}
