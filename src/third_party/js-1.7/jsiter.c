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
 * JavaScript iterators.
 */
#include "jsstddef.h"
#include <string.h>     /* for memcpy */
#include "jstypes.h"
#include "jsutil.h"
#include "jsarena.h"
#include "jsapi.h"
#include "jsarray.h"
#include "jsatom.h"
#include "jsbool.h"
#include "jscntxt.h"
#include "jsconfig.h"
#include "jsexn.h"
#include "jsfun.h"
#include "jsgc.h"
#include "jsinterp.h"
#include "jsiter.h"
#include "jslock.h"
#include "jsnum.h"
#include "jsobj.h"
#include "jsopcode.h"
#include "jsscope.h"
#include "jsscript.h"

#if JS_HAS_XML_SUPPORT
#include "jsxml.h"
#endif

extern const char js_throw_str[]; /* from jsscan.h */

#define JSSLOT_ITER_STATE       (JSSLOT_PRIVATE)
#define JSSLOT_ITER_FLAGS       (JSSLOT_PRIVATE + 1)

#if JSSLOT_ITER_FLAGS >= JS_INITIAL_NSLOTS
#error JS_INITIAL_NSLOTS must be greater than JSSLOT_ITER_FLAGS.
#endif

/*
 * Shared code to close iterator's state either through an explicit call or
 * when GC detects that the iterator is no longer reachable.
 */
void
js_CloseIteratorState(JSContext *cx, JSObject *iterobj)
{
    jsval *slots;
    jsval state, parent;
    JSObject *iterable;

    JS_ASSERT(JS_InstanceOf(cx, iterobj, &js_IteratorClass, NULL));
    slots = iterobj->slots;

    /* Avoid double work if js_CloseNativeIterator was called on obj. */
    state = slots[JSSLOT_ITER_STATE];
    if (JSVAL_IS_NULL(state))
        return;

    /* Protect against failure to fully initialize obj. */
    parent = slots[JSSLOT_PARENT];
    if (!JSVAL_IS_PRIMITIVE(parent)) {
        iterable = JSVAL_TO_OBJECT(parent);
#if JS_HAS_XML_SUPPORT
        if ((JSVAL_TO_INT(slots[JSSLOT_ITER_FLAGS]) & JSITER_FOREACH) &&
            OBJECT_IS_XML(cx, iterable)) {
            ((JSXMLObjectOps *) iterable->map->ops)->
                enumerateValues(cx, iterable, JSENUMERATE_DESTROY, &state,
                                NULL, NULL);
        } else
#endif
            OBJ_ENUMERATE(cx, iterable, JSENUMERATE_DESTROY, &state, NULL);
    }
    slots[JSSLOT_ITER_STATE] = JSVAL_NULL;
}

JSClass js_IteratorClass = {
    "Iterator",
    JSCLASS_HAS_RESERVED_SLOTS(2) | /* slots for state and flags */
    JSCLASS_HAS_CACHED_PROTO(JSProto_Iterator),
    JS_PropertyStub,  JS_PropertyStub,  JS_PropertyStub,  JS_PropertyStub,
    JS_EnumerateStub, JS_ResolveStub,   JS_ConvertStub,   JS_FinalizeStub,
    JSCLASS_NO_OPTIONAL_MEMBERS
};

static JSBool
InitNativeIterator(JSContext *cx, JSObject *iterobj, JSObject *obj, uintN flags)
{
    jsval state;
    JSBool ok;

    JS_ASSERT(JSVAL_TO_PRIVATE(iterobj->slots[JSSLOT_CLASS]) ==
              &js_IteratorClass);

    /* Initialize iterobj in case of enumerate hook failure. */
    iterobj->slots[JSSLOT_PARENT] = OBJECT_TO_JSVAL(obj);
    iterobj->slots[JSSLOT_ITER_STATE] = JSVAL_NULL;
    iterobj->slots[JSSLOT_ITER_FLAGS] = INT_TO_JSVAL(flags);
    if (!js_RegisterCloseableIterator(cx, iterobj))
        return JS_FALSE;
    if (!obj)
        return JS_TRUE;

    ok =
#if JS_HAS_XML_SUPPORT
         ((flags & JSITER_FOREACH) && OBJECT_IS_XML(cx, obj))
         ? ((JSXMLObjectOps *) obj->map->ops)->
               enumerateValues(cx, obj, JSENUMERATE_INIT, &state, NULL, NULL)
         :
#endif
           OBJ_ENUMERATE(cx, obj, JSENUMERATE_INIT, &state, NULL);
    if (!ok)
        return JS_FALSE;

    iterobj->slots[JSSLOT_ITER_STATE] = state;
    if (flags & JSITER_ENUMERATE) {
        /*
         * The enumerating iterator needs the original object to suppress
         * enumeration of deleted or shadowed prototype properties. Since the
         * enumerator never escapes to scripts, we use the prototype slot to
         * store the original object.
         */
        JS_ASSERT(obj != iterobj);
        iterobj->slots[JSSLOT_PROTO] = OBJECT_TO_JSVAL(obj);
    }
    return JS_TRUE;
}

static JSBool
Iterator(JSContext *cx, JSObject *iterobj, uintN argc, jsval *argv, jsval *rval)
{
    JSBool keyonly;
    uintN flags;
    JSObject *obj;

    keyonly = JS_FALSE;
    if (!js_ValueToBoolean(cx, argv[1], &keyonly))
        return JS_FALSE;
    flags = keyonly ? 0 : JSITER_FOREACH;

    if (cx->fp->flags & JSFRAME_CONSTRUCTING) {
        /* XXX work around old valueOf call hidden beneath js_ValueToObject */
        if (!JSVAL_IS_PRIMITIVE(argv[0])) {
            obj = JSVAL_TO_OBJECT(argv[0]);
        } else {
            obj = js_ValueToNonNullObject(cx, argv[0]);
            if (!obj)
                return JS_FALSE;
            argv[0] = OBJECT_TO_JSVAL(obj);
        }
        return InitNativeIterator(cx, iterobj, obj, flags);
    }

    *rval = argv[0];
    return js_ValueToIterator(cx, flags, rval);
}

static JSBool
NewKeyValuePair(JSContext *cx, jsid key, jsval val, jsval *rval)
{
    jsval vec[2];
    JSTempValueRooter tvr;
    JSObject *aobj;

    vec[0] = ID_TO_VALUE(key);
    vec[1] = val;

    JS_PUSH_TEMP_ROOT(cx, 2, vec, &tvr);
    aobj = js_NewArrayObject(cx, 2, vec);
    *rval = OBJECT_TO_JSVAL(aobj);
    JS_POP_TEMP_ROOT(cx, &tvr);

    return aobj != NULL;
}

static JSBool
IteratorNextImpl(JSContext *cx, JSObject *obj, jsval *rval)
{
    JSObject *iterable;
    jsval state;
    uintN flags;
    JSBool foreach, ok;
    jsid id;

    JS_ASSERT(OBJ_GET_CLASS(cx, obj) == &js_IteratorClass);

    iterable = OBJ_GET_PARENT(cx, obj);
    JS_ASSERT(iterable);
    state = OBJ_GET_SLOT(cx, obj, JSSLOT_ITER_STATE);
    if (JSVAL_IS_NULL(state))
        goto stop;

    flags = JSVAL_TO_INT(OBJ_GET_SLOT(cx, obj, JSSLOT_ITER_FLAGS));
    JS_ASSERT(!(flags & JSITER_ENUMERATE));
    foreach = (flags & JSITER_FOREACH) != 0;
    ok =
#if JS_HAS_XML_SUPPORT
         (foreach && OBJECT_IS_XML(cx, iterable))
         ? ((JSXMLObjectOps *) iterable->map->ops)->
               enumerateValues(cx, iterable, JSENUMERATE_NEXT, &state,
                               &id, rval)
         :
#endif
           OBJ_ENUMERATE(cx, iterable, JSENUMERATE_NEXT, &state, &id);
    if (!ok)
        return JS_FALSE;

    OBJ_SET_SLOT(cx, obj, JSSLOT_ITER_STATE, state);
    if (JSVAL_IS_NULL(state))
        goto stop;

    if (foreach) {
#if JS_HAS_XML_SUPPORT
        if (!OBJECT_IS_XML(cx, iterable) &&
            !OBJ_GET_PROPERTY(cx, iterable, id, rval)) {
            return JS_FALSE;
        }
#endif
        if (!NewKeyValuePair(cx, id, *rval, rval))
            return JS_FALSE;
    } else {
        *rval = ID_TO_VALUE(id);
    }
    return JS_TRUE;

  stop:
    JS_ASSERT(OBJ_GET_SLOT(cx, obj, JSSLOT_ITER_STATE) == JSVAL_NULL);
    *rval = JSVAL_HOLE;
    return JS_TRUE;
}

static JSBool
js_ThrowStopIteration(JSContext *cx, JSObject *obj)
{
    jsval v;

    JS_ASSERT(!JS_IsExceptionPending(cx));
    if (js_FindClassObject(cx, NULL, INT_TO_JSID(JSProto_StopIteration), &v))
        JS_SetPendingException(cx, v);
    return JS_FALSE;
}

static JSBool
iterator_next(JSContext *cx, JSObject *obj, uintN argc, jsval *argv,
              jsval *rval)
{
    if (!JS_InstanceOf(cx, obj, &js_IteratorClass, argv))
        return JS_FALSE;

    if (!IteratorNextImpl(cx, obj, rval))
        return JS_FALSE;

    if (*rval == JSVAL_HOLE) {
        *rval = JSVAL_NULL;
        js_ThrowStopIteration(cx, obj);
        return JS_FALSE;
    }
    return JS_TRUE;
}

static JSBool
iterator_self(JSContext *cx, JSObject *obj, uintN argc, jsval *argv,
              jsval *rval)
{
    *rval = OBJECT_TO_JSVAL(obj);
    return JS_TRUE;
}

static JSFunctionSpec iterator_methods[] = {
    {js_iterator_str, iterator_self, 0,JSPROP_READONLY|JSPROP_PERMANENT,0},
    {js_next_str,     iterator_next, 0,JSPROP_READONLY|JSPROP_PERMANENT,0},
    {0,0,0,0,0}
};

uintN
js_GetNativeIteratorFlags(JSContext *cx, JSObject *iterobj)
{
    if (OBJ_GET_CLASS(cx, iterobj) != &js_IteratorClass)
        return 0;
    return JSVAL_TO_INT(OBJ_GET_SLOT(cx, iterobj, JSSLOT_ITER_FLAGS));
}

void
js_CloseNativeIterator(JSContext *cx, JSObject *iterobj)
{
    uintN flags;

    /*
     * If this iterator is not an instance of the native default iterator
     * class, leave it to be GC'ed.
     */
    if (!JS_InstanceOf(cx, iterobj, &js_IteratorClass, NULL))
        return;

    /*
     * If this iterator was not created by js_ValueToIterator called from the
     * for-in loop code in js_Interpret, leave it to be GC'ed.
     */
    flags = JSVAL_TO_INT(OBJ_GET_SLOT(cx, iterobj, JSSLOT_ITER_FLAGS));
    if (!(flags & JSITER_ENUMERATE))
        return;

    js_CloseIteratorState(cx, iterobj);
}

/*
 * Call ToObject(v).__iterator__(keyonly) if ToObject(v).__iterator__ exists.
 * Otherwise construct the defualt iterator.
 */
JSBool
js_ValueToIterator(JSContext *cx, uintN flags, jsval *vp)
{
    JSObject *obj;
    JSTempValueRooter tvr;
    const JSAtom *atom;
    JSBool ok;
    JSObject *iterobj;
    jsval arg;
    JSString *str;

    JS_ASSERT(!(flags & ~(JSITER_ENUMERATE |
                          JSITER_FOREACH |
                          JSITER_KEYVALUE)));

    /* JSITER_KEYVALUE must always come with JSITER_FOREACH */
    JS_ASSERT(!(flags & JSITER_KEYVALUE) || (flags & JSITER_FOREACH));

    /* XXX work around old valueOf call hidden beneath js_ValueToObject */
    if (!JSVAL_IS_PRIMITIVE(*vp)) {
        obj = JSVAL_TO_OBJECT(*vp);
    } else {
        /*
         * Enumerating over null and undefined gives an empty enumerator.
         * This is contrary to ECMA-262 9.9 ToObject, invoked from step 3 of
         * the first production in 12.6.4 and step 4 of the second production,
         * but it's "web JS" compatible.
         */
        if ((flags & JSITER_ENUMERATE)) {
            if (!js_ValueToObject(cx, *vp, &obj))
                return JS_FALSE;
            if (!obj)
                goto default_iter;
        } else {
            obj = js_ValueToNonNullObject(cx, *vp);
            if (!obj)
                return JS_FALSE;
        }
    }

    JS_ASSERT(obj);
    JS_PUSH_TEMP_ROOT_OBJECT(cx, obj, &tvr);

    atom = cx->runtime->atomState.iteratorAtom;
#if JS_HAS_XML_SUPPORT
    if (OBJECT_IS_XML(cx, obj)) {
        if (!js_GetXMLFunction(cx, obj, ATOM_TO_JSID(atom), vp))
            goto bad;
    } else
#endif
    {
        if (!OBJ_GET_PROPERTY(cx, obj, ATOM_TO_JSID(atom), vp))
            goto bad;
    }

    if (JSVAL_IS_VOID(*vp)) {
      default_iter:
        /*
         * Fail over to the default enumerating native iterator.
         *
         * Create iterobj with a NULL parent to ensure that we use the correct
         * scope chain to lookup the iterator's constructor. Since we use the
         * parent slot to keep track of the iterable, we must fix it up after.
         */
        iterobj = js_NewObject(cx, &js_IteratorClass, NULL, NULL);
        if (!iterobj)
            goto bad;

        /* Store iterobj in *vp to protect it from GC (callers must root vp). */
        *vp = OBJECT_TO_JSVAL(iterobj);

        if (!InitNativeIterator(cx, iterobj, obj, flags))
            goto bad;
    } else {
        arg = BOOLEAN_TO_JSVAL((flags & JSITER_FOREACH) == 0);
        if (!js_InternalInvoke(cx, obj, *vp, JSINVOKE_ITERATOR, 1, &arg, vp))
            goto bad;
        if (JSVAL_IS_PRIMITIVE(*vp)) {
            str = js_DecompileValueGenerator(cx, JSDVG_SEARCH_STACK, *vp, NULL);
            if (str) {
                JS_ReportErrorNumberUC(cx, js_GetErrorMessage, NULL,
                                       JSMSG_BAD_ITERATOR_RETURN,
                                       JSSTRING_CHARS(str),
                                       JSSTRING_CHARS(ATOM_TO_STRING(atom)));
            }
            goto bad;
        }
    }

    ok = JS_TRUE;
  out:
    if (obj)
        JS_POP_TEMP_ROOT(cx, &tvr);
    return ok;
  bad:
    ok = JS_FALSE;
    goto out;
}

static JSBool
CallEnumeratorNext(JSContext *cx, JSObject *iterobj, uintN flags, jsval *rval)
{
    JSObject *obj, *origobj;
    jsval state;
    JSBool foreach;
    jsid id;
    JSObject *obj2;
    JSBool cond;
    JSClass *clasp;
    JSExtendedClass *xclasp;
    JSProperty *prop;
    JSString *str;

    JS_ASSERT(flags & JSITER_ENUMERATE);
    JS_ASSERT(JSVAL_TO_PRIVATE(iterobj->slots[JSSLOT_CLASS]) ==
              &js_IteratorClass);

    obj = JSVAL_TO_OBJECT(iterobj->slots[JSSLOT_PARENT]);
    origobj = JSVAL_TO_OBJECT(iterobj->slots[JSSLOT_PROTO]);
    state = iterobj->slots[JSSLOT_ITER_STATE];
    if (JSVAL_IS_NULL(state))
        goto stop;

    foreach = (flags & JSITER_FOREACH) != 0;
#if JS_HAS_XML_SUPPORT
    /*
     * Treat an XML object specially only when it starts the prototype chain.
     * Otherwise we need to do the usual deleted and shadowed property checks.
     */
    if (obj == origobj && OBJECT_IS_XML(cx, obj)) {
        if (foreach) {
            JSXMLObjectOps *xmlops = (JSXMLObjectOps *) obj->map->ops;

            if (!xmlops->enumerateValues(cx, obj, JSENUMERATE_NEXT, &state,
                                         &id, rval)) {
                return JS_FALSE;
            }
        } else {
            if (!OBJ_ENUMERATE(cx, obj, JSENUMERATE_NEXT, &state, &id))
                return JS_FALSE;
        }
        iterobj->slots[JSSLOT_ITER_STATE] = state;
        if (JSVAL_IS_NULL(state))
            goto stop;
    } else
#endif
    {
      restart:
        if (!OBJ_ENUMERATE(cx, obj, JSENUMERATE_NEXT, &state, &id))
            return JS_TRUE;

        iterobj->slots[JSSLOT_ITER_STATE] = state;
        if (JSVAL_IS_NULL(state)) {
#if JS_HAS_XML_SUPPORT
            if (OBJECT_IS_XML(cx, obj)) {
                /*
                 * We just finished enumerating an XML obj that is present on
                 * the prototype chain of a non-XML origobj. Stop further
                 * prototype chain searches because XML objects don't
                 * enumerate prototypes.
                 */
                JS_ASSERT(origobj != obj);
                JS_ASSERT(!OBJECT_IS_XML(cx, origobj));
            } else
#endif
            {
                obj = OBJ_GET_PROTO(cx, obj);
                if (obj) {
                    iterobj->slots[JSSLOT_PARENT] = OBJECT_TO_JSVAL(obj);
                    if (!OBJ_ENUMERATE(cx, obj, JSENUMERATE_INIT, &state, NULL))
                        return JS_FALSE;
                    iterobj->slots[JSSLOT_ITER_STATE] = state;
                    if (!JSVAL_IS_NULL(state))
                        goto restart;
                }
            }
            goto stop;
        }

        /* Skip properties not in obj when looking from origobj. */
        if (!OBJ_LOOKUP_PROPERTY(cx, origobj, id, &obj2, &prop))
            return JS_FALSE;
        if (!prop)
            goto restart;
        OBJ_DROP_PROPERTY(cx, obj2, prop);

        /*
         * If the id was found in a prototype object or an unrelated object
         * (specifically, not in an inner object for obj), skip it. This step
         * means that all OBJ_LOOKUP_PROPERTY implementations must return an
         * object further along on the prototype chain, or else possibly an
         * object returned by the JSExtendedClass.outerObject optional hook.
         */
        if (obj != obj2) {
            cond = JS_FALSE;
            clasp = OBJ_GET_CLASS(cx, obj2);
            if (clasp->flags & JSCLASS_IS_EXTENDED) {
                xclasp = (JSExtendedClass *) clasp;
                cond = xclasp->outerObject &&
                    xclasp->outerObject(cx, obj2) == obj;
            }
            if (!cond)
                goto restart;
        }

        if (foreach) {
            /* Get property querying the original object. */
            if (!OBJ_GET_PROPERTY(cx, origobj, id, rval))
                return JS_FALSE;
        }
    }

    if (foreach) {
        if (flags & JSITER_KEYVALUE) {
            if (!NewKeyValuePair(cx, id, *rval, rval))
                return JS_FALSE;
        }
    } else {
        /* Make rval a string for uniformity and compatibility. */
        if (JSID_IS_ATOM(id)) {
            *rval = ATOM_KEY(JSID_TO_ATOM(id));
        }
#if JS_HAS_XML_SUPPORT
        else if (JSID_IS_OBJECT(id)) {
            str = js_ValueToString(cx, OBJECT_JSID_TO_JSVAL(id));
            if (!str)
                return JS_FALSE;
            *rval = STRING_TO_JSVAL(str);
        }
#endif
        else {
            str = js_NumberToString(cx, (jsdouble)JSID_TO_INT(id));
            if (!str)
                return JS_FALSE;
            *rval = STRING_TO_JSVAL(str);
        }
    }
    return JS_TRUE;

  stop:
    JS_ASSERT(iterobj->slots[JSSLOT_ITER_STATE] == JSVAL_NULL);
    *rval = JSVAL_HOLE;
    return JS_TRUE;
}

JSBool
js_CallIteratorNext(JSContext *cx, JSObject *iterobj, jsval *rval)
{
    uintN flags;

    /* Fast path for native iterators */
    if (OBJ_GET_CLASS(cx, iterobj) == &js_IteratorClass) {
        flags = JSVAL_TO_INT(OBJ_GET_SLOT(cx, iterobj, JSSLOT_ITER_FLAGS));
        if (flags & JSITER_ENUMERATE)
            return CallEnumeratorNext(cx, iterobj, flags, rval);

        /*
         * Call next directly as all the methods of the native iterator are
         * read-only and permanent.
         */
        if (!IteratorNextImpl(cx, iterobj, rval))
            return JS_FALSE;
    } else {
        jsid id = ATOM_TO_JSID(cx->runtime->atomState.nextAtom);

        if (!JS_GetMethodById(cx, iterobj, id, &iterobj, rval))
            return JS_FALSE;
        if (!js_InternalCall(cx, iterobj, *rval, 0, NULL, rval)) {
            /* Check for StopIteration. */
            if (!cx->throwing ||
                JSVAL_IS_PRIMITIVE(cx->exception) ||
                OBJ_GET_CLASS(cx, JSVAL_TO_OBJECT(cx->exception))
                    != &js_StopIterationClass) {
                return JS_FALSE;
            }

            /* Inline JS_ClearPendingException(cx). */
            cx->throwing = JS_FALSE;
            cx->exception = JSVAL_VOID;
            *rval = JSVAL_HOLE;
            return JS_TRUE;
        }
    }

    return JS_TRUE;
}

static JSBool
stopiter_hasInstance(JSContext *cx, JSObject *obj, jsval v, JSBool *bp)
{
    *bp = !JSVAL_IS_PRIMITIVE(v) &&
          OBJ_GET_CLASS(cx, JSVAL_TO_OBJECT(v)) == &js_StopIterationClass;
    return JS_TRUE;
}

JSClass js_StopIterationClass = {
    js_StopIteration_str,
    JSCLASS_HAS_CACHED_PROTO(JSProto_StopIteration),
    JS_PropertyStub,  JS_PropertyStub,
    JS_PropertyStub,  JS_PropertyStub,
    JS_EnumerateStub, JS_ResolveStub,
    JS_ConvertStub,   JS_FinalizeStub,
    NULL,             NULL,
    NULL,             NULL,
    NULL,             stopiter_hasInstance,
    NULL,             NULL
};

#if JS_HAS_GENERATORS

static void
generator_finalize(JSContext *cx, JSObject *obj)
{
    JSGenerator *gen;

    gen = (JSGenerator *) JS_GetPrivate(cx, obj);
    if (gen) {
        /*
         * gen can be open on shutdown when close hooks are ignored or when
         * the embedding cancels scheduled close hooks.
         */
        JS_ASSERT(gen->state == JSGEN_NEWBORN || gen->state == JSGEN_CLOSED ||
                  gen->state == JSGEN_OPEN);
        JS_free(cx, gen);
    }
}

static uint32
generator_mark(JSContext *cx, JSObject *obj, void *arg)
{
    JSGenerator *gen;

    gen = (JSGenerator *) JS_GetPrivate(cx, obj);
    if (gen) {
        /*
         * We must mark argv[-2], as js_MarkStackFrame will not.  Note that
         * js_MarkStackFrame will mark thisp (argv[-1]) and actual arguments,
         * plus any missing formals and local GC roots.
         */
        JS_ASSERT(!JSVAL_IS_PRIMITIVE(gen->frame.argv[-2]));
        GC_MARK(cx, JSVAL_TO_GCTHING(gen->frame.argv[-2]), "generator");
        js_MarkStackFrame(cx, &gen->frame);
    }
    return 0;
}

JSClass js_GeneratorClass = {
    js_Generator_str,
    JSCLASS_HAS_PRIVATE | JSCLASS_IS_ANONYMOUS |
    JSCLASS_HAS_CACHED_PROTO(JSProto_Generator),
    JS_PropertyStub,  JS_PropertyStub, JS_PropertyStub, JS_PropertyStub,
    JS_EnumerateStub, JS_ResolveStub,  JS_ConvertStub,  generator_finalize,
    NULL,             NULL,            NULL,            NULL,
    NULL,             NULL,            generator_mark,  NULL
};

/*
 * Called from the JSOP_GENERATOR case in the interpreter, with fp referring
 * to the frame by which the generator function was activated.  Create a new
 * JSGenerator object, which contains its own JSStackFrame that we populate
 * from *fp.  We know that upon return, the JSOP_GENERATOR opcode will return
 * from the activation in fp, so we can steal away fp->callobj and fp->argsobj
 * if they are non-null.
 */
JSObject *
js_NewGenerator(JSContext *cx, JSStackFrame *fp)
{
    JSObject *obj;
    uintN argc, nargs, nvars, depth, nslots;
    JSGenerator *gen;
    jsval *newsp;

    /* After the following return, failing control flow must goto bad. */
    obj = js_NewObject(cx, &js_GeneratorClass, NULL, NULL);
    if (!obj)
        return NULL;

    /* Load and compute stack slot counts. */
    argc = fp->argc;
    nargs = JS_MAX(argc, fp->fun->nargs);
    nvars = fp->nvars;
    depth = fp->script->depth;
    nslots = 2 + nargs + nvars + 2 * depth;

    /* Allocate obj's private data struct. */
    gen = (JSGenerator *)
          JS_malloc(cx, sizeof(JSGenerator) + (nslots - 1) * sizeof(jsval));
    if (!gen)
        goto bad;

    gen->obj = obj;

    /* Steal away objects reflecting fp and point them at gen->frame. */
    gen->frame.callobj = fp->callobj;
    if (fp->callobj) {
        JS_SetPrivate(cx, fp->callobj, &gen->frame);
        fp->callobj = NULL;
    }
    gen->frame.argsobj = fp->argsobj;
    if (fp->argsobj) {
        JS_SetPrivate(cx, fp->argsobj, &gen->frame);
        fp->argsobj = NULL;
    }

    /* These two references can be shared with fp until it goes away. */
    gen->frame.varobj = fp->varobj;
    gen->frame.thisp = fp->thisp;

    /* Copy call-invariant script and function references. */
    gen->frame.script = fp->script;
    gen->frame.fun = fp->fun;

    /* Use newsp to carve space out of gen->stack. */
    newsp = gen->stack;
    gen->arena.next = NULL;
    gen->arena.base = (jsuword) newsp;
    gen->arena.limit = gen->arena.avail = (jsuword) (newsp + nslots);

#define COPY_STACK_ARRAY(vec,cnt,num)                                         \
    JS_BEGIN_MACRO                                                            \
        gen->frame.cnt = cnt;                                                 \
        gen->frame.vec = newsp;                                               \
        newsp += (num);                                                       \
        memcpy(gen->frame.vec, fp->vec, (num) * sizeof(jsval));               \
    JS_END_MACRO

    /* Copy argv, rval, and vars. */
    *newsp++ = fp->argv[-2];
    *newsp++ = fp->argv[-1];
    COPY_STACK_ARRAY(argv, argc, nargs);
    gen->frame.rval = fp->rval;
    COPY_STACK_ARRAY(vars, nvars, nvars);

#undef COPY_STACK_ARRAY

    /* Initialize or copy virtual machine state. */
    gen->frame.down = NULL;
    gen->frame.annotation = NULL;
    gen->frame.scopeChain = fp->scopeChain;
    gen->frame.pc = fp->pc;

    /* Allocate generating pc and operand stack space. */
    gen->frame.spbase = gen->frame.sp = newsp + depth;

    /* Copy remaining state (XXX sharp* and xml* should be local vars). */
    gen->frame.sharpDepth = 0;
    gen->frame.sharpArray = NULL;
    gen->frame.flags = fp->flags | JSFRAME_GENERATOR;
    gen->frame.dormantNext = NULL;
    gen->frame.xmlNamespace = NULL;
    gen->frame.blockChain = NULL;

    /* Note that gen is newborn. */
    gen->state = JSGEN_NEWBORN;

    if (!JS_SetPrivate(cx, obj, gen)) {
        JS_free(cx, gen);
        goto bad;
    }

    /*
     * Register with GC to ensure that suspended finally blocks will be
     * executed.
     */
    js_RegisterGenerator(cx, gen);
    return obj;

  bad:
    cx->weakRoots.newborn[GCX_OBJECT] = NULL;
    return NULL;
}

typedef enum JSGeneratorOp {
    JSGENOP_NEXT,
    JSGENOP_SEND,
    JSGENOP_THROW,
    JSGENOP_CLOSE
} JSGeneratorOp;

/*
 * Start newborn or restart yielding generator and perform the requested
 * operation inside its frame.
 */
static JSBool
SendToGenerator(JSContext *cx, JSGeneratorOp op, JSObject *obj,
                JSGenerator *gen, jsval arg, jsval *rval)
{
    JSStackFrame *fp;
    jsval junk;
    JSArena *arena;
    JSBool ok;

    JS_ASSERT(gen->state ==  JSGEN_NEWBORN || gen->state == JSGEN_OPEN);
    switch (op) {
      case JSGENOP_NEXT:
      case JSGENOP_SEND:
        if (gen->state == JSGEN_OPEN) {
            /*
             * Store the argument to send as the result of the yield
             * expression.
             */
            gen->frame.sp[-1] = arg;
        }
        gen->state = JSGEN_RUNNING;
        break;

      case JSGENOP_THROW:
        JS_SetPendingException(cx, arg);
        gen->state = JSGEN_RUNNING;
        break;

      default:
        JS_ASSERT(op == JSGENOP_CLOSE);
        JS_SetPendingException(cx, JSVAL_ARETURN);
        gen->state = JSGEN_CLOSING;
        break;
    }

    /* Extend the current stack pool with gen->arena. */
    arena = cx->stackPool.current;
    JS_ASSERT(!arena->next);
    JS_ASSERT(!gen->arena.next);
    JS_ASSERT(cx->stackPool.current != &gen->arena);
    cx->stackPool.current = arena->next = &gen->arena;

    /* Push gen->frame around the interpreter activation. */
    fp = cx->fp;
    cx->fp = &gen->frame;
    gen->frame.down = fp;
    ok = js_Interpret(cx, gen->frame.pc, &junk);
    cx->fp = fp;
    gen->frame.down = NULL;

    /* Retract the stack pool and sanitize gen->arena. */
    JS_ASSERT(!gen->arena.next);
    JS_ASSERT(arena->next == &gen->arena);
    JS_ASSERT(cx->stackPool.current == &gen->arena);
    cx->stackPool.current = arena;
    arena->next = NULL;

    if (gen->frame.flags & JSFRAME_YIELDING) {
        /* Yield cannot fail, throw or be called on closing. */
        JS_ASSERT(ok);
        JS_ASSERT(!cx->throwing);
        JS_ASSERT(gen->state == JSGEN_RUNNING);
        JS_ASSERT(op != JSGENOP_CLOSE);
        gen->frame.flags &= ~JSFRAME_YIELDING;
        gen->state = JSGEN_OPEN;
        *rval = gen->frame.rval;
        return JS_TRUE;
    }

    gen->state = JSGEN_CLOSED;

    if (ok) {
        /* Returned, explicitly or by falling off the end. */
        if (op == JSGENOP_CLOSE)
            return JS_TRUE;
        return js_ThrowStopIteration(cx, obj);
    }

    /*
     * An error, silent termination by branch callback or an exception.
     * Propagate the condition to the caller.
     */
    return JS_FALSE;
}

/*
 * Execute gen's close hook after the GC detects that the object has become
 * unreachable.
 */
JSBool
js_CloseGeneratorObject(JSContext *cx, JSGenerator *gen)
{
    /* We pass null as rval since SendToGenerator never uses it with CLOSE. */
    return SendToGenerator(cx, JSGENOP_CLOSE, gen->obj, gen, JSVAL_VOID, NULL);
}

/*
 * Common subroutine of generator_(next|send|throw|close) methods.
 */
static JSBool
generator_op(JSContext *cx, JSGeneratorOp op,
             JSObject *obj, uintN argc, jsval *argv, jsval *rval)
{
    JSGenerator *gen;
    JSString *str;
    jsval arg;

    if (!JS_InstanceOf(cx, obj, &js_GeneratorClass, argv))
        return JS_FALSE;

    gen = (JSGenerator *) JS_GetPrivate(cx, obj);
    if (gen == NULL) {
        /* This happens when obj is the generator prototype. See bug 352885. */
        goto closed_generator;
    }

    switch (gen->state) {
      case JSGEN_NEWBORN:
        switch (op) {
          case JSGENOP_NEXT:
          case JSGENOP_THROW:
            break;

          case JSGENOP_SEND:
            if (!JSVAL_IS_VOID(argv[0])) {
                str = js_DecompileValueGenerator(cx, JSDVG_SEARCH_STACK,
                                                 argv[0], NULL);
                if (str) {
                    JS_ReportErrorNumberUC(cx, js_GetErrorMessage, NULL,
                                           JSMSG_BAD_GENERATOR_SEND,
                                           JSSTRING_CHARS(str));
                }
                return JS_FALSE;
            }
            break;

          default:
            JS_ASSERT(op == JSGENOP_CLOSE);
            gen->state = JSGEN_CLOSED;
            return JS_TRUE;
        }
        break;

      case JSGEN_OPEN:
        break;

      case JSGEN_RUNNING:
      case JSGEN_CLOSING:
        str = js_DecompileValueGenerator(cx, JSDVG_SEARCH_STACK, argv[-1],
                                         JS_GetFunctionId(gen->frame.fun));
        if (str) {
            JS_ReportErrorNumberUC(cx, js_GetErrorMessage, NULL,
                                   JSMSG_NESTING_GENERATOR,
                                   JSSTRING_CHARS(str));
        }
        return JS_FALSE;

      default:
        JS_ASSERT(gen->state == JSGEN_CLOSED);

      closed_generator:
        switch (op) {
          case JSGENOP_NEXT:
          case JSGENOP_SEND:
            return js_ThrowStopIteration(cx, obj);
          case JSGENOP_THROW:
            JS_SetPendingException(cx, argv[0]);
            return JS_FALSE;
          default:
            JS_ASSERT(op == JSGENOP_CLOSE);
            return JS_TRUE;
        }
    }

    arg = (op == JSGENOP_SEND || op == JSGENOP_THROW)
          ? argv[0]
          : JSVAL_VOID;
    if (!SendToGenerator(cx, op, obj, gen, arg, rval))
        return JS_FALSE;
    return JS_TRUE;
}

static JSBool
generator_send(JSContext *cx, JSObject *obj, uintN argc, jsval *argv,
               jsval *rval)
{
    return generator_op(cx, JSGENOP_SEND, obj, argc, argv, rval);
}

static JSBool
generator_next(JSContext *cx, JSObject *obj, uintN argc, jsval *argv,
               jsval *rval)
{
    return generator_op(cx, JSGENOP_NEXT, obj, argc, argv, rval);
}

static JSBool
generator_throw(JSContext *cx, JSObject *obj, uintN argc, jsval *argv,
                jsval *rval)
{
    return generator_op(cx, JSGENOP_THROW, obj, argc, argv, rval);
}

static JSBool
generator_close(JSContext *cx, JSObject *obj, uintN argc, jsval *argv,
                jsval *rval)
{
    return generator_op(cx, JSGENOP_CLOSE, obj, argc, argv, rval);
}

static JSFunctionSpec generator_methods[] = {
    {js_iterator_str, iterator_self,     0,JSPROP_READONLY|JSPROP_PERMANENT,0},
    {js_next_str,     generator_next,    0,JSPROP_READONLY|JSPROP_PERMANENT,0},
    {js_send_str,     generator_send,    1,JSPROP_READONLY|JSPROP_PERMANENT,0},
    {js_throw_str,    generator_throw,   1,JSPROP_READONLY|JSPROP_PERMANENT,0},
    {js_close_str,    generator_close,   0,JSPROP_READONLY|JSPROP_PERMANENT,0},
    {0,0,0,0,0}
};

#endif /* JS_HAS_GENERATORS */

JSObject *
js_InitIteratorClasses(JSContext *cx, JSObject *obj)
{
    JSObject *proto, *stop;

    /* Idempotency required: we initialize several things, possibly lazily. */
    if (!js_GetClassObject(cx, obj, JSProto_StopIteration, &stop))
        return NULL;
    if (stop)
        return stop;

    proto = JS_InitClass(cx, obj, NULL, &js_IteratorClass, Iterator, 2,
                         NULL, iterator_methods, NULL, NULL);
    if (!proto)
        return NULL;
    proto->slots[JSSLOT_ITER_STATE] = JSVAL_NULL;

#if JS_HAS_GENERATORS
    /* Initialize the generator internals if configured. */
    if (!JS_InitClass(cx, obj, NULL, &js_GeneratorClass, NULL, 0,
                      NULL, generator_methods, NULL, NULL)) {
        return NULL;
    }
#endif

    return JS_InitClass(cx, obj, NULL, &js_StopIterationClass, NULL, 0,
                        NULL, NULL, NULL, NULL);
}
