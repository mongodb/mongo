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
 * JS object implementation.
 */
#include "jsstddef.h"
#include <stdlib.h>
#include <string.h>
#include "jstypes.h"
#include "jsarena.h" /* Added by JSIFY */
#include "jsbit.h"
#include "jsutil.h" /* Added by JSIFY */
#include "jshash.h" /* Added by JSIFY */
#include "jsdhash.h"
#include "jsprf.h"
#include "jsapi.h"
#include "jsarray.h"
#include "jsatom.h"
#include "jsbool.h"
#include "jscntxt.h"
#include "jsconfig.h"
#include "jsfun.h"
#include "jsgc.h"
#include "jsinterp.h"
#include "jslock.h"
#include "jsnum.h"
#include "jsobj.h"
#include "jsscan.h"
#include "jsscope.h"
#include "jsscript.h"
#include "jsstr.h"
#include "jsopcode.h"

#include "jsdbgapi.h"   /* whether or not JS_HAS_OBJ_WATCHPOINT */

#if JS_HAS_GENERATORS
#include "jsiter.h"
#endif

#if JS_HAS_XML_SUPPORT
#include "jsxml.h"
#endif

#if JS_HAS_XDR
#include "jsxdrapi.h"
#endif

#ifdef JS_THREADSAFE
#define NATIVE_DROP_PROPERTY js_DropProperty

extern void
js_DropProperty(JSContext *cx, JSObject *obj, JSProperty *prop);
#else
#define NATIVE_DROP_PROPERTY NULL
#endif

JS_FRIEND_DATA(JSObjectOps) js_ObjectOps = {
    js_NewObjectMap,        js_DestroyObjectMap,
    js_LookupProperty,      js_DefineProperty,
    js_GetProperty,         js_SetProperty,
    js_GetAttributes,       js_SetAttributes,
    js_DeleteProperty,      js_DefaultValue,
    js_Enumerate,           js_CheckAccess,
    NULL,                   NATIVE_DROP_PROPERTY,
    js_Call,                js_Construct,
    NULL,                   js_HasInstance,
    js_SetProtoOrParent,    js_SetProtoOrParent,
    js_Mark,                js_Clear,
    js_GetRequiredSlot,     js_SetRequiredSlot
};

JSClass js_ObjectClass = {
    js_Object_str,
    JSCLASS_HAS_CACHED_PROTO(JSProto_Object),
    JS_PropertyStub,  JS_PropertyStub,  JS_PropertyStub,  JS_PropertyStub,
    JS_EnumerateStub, JS_ResolveStub,   JS_ConvertStub,   JS_FinalizeStub,
    JSCLASS_NO_OPTIONAL_MEMBERS
};

#if JS_HAS_OBJ_PROTO_PROP

static JSBool
obj_getSlot(JSContext *cx, JSObject *obj, jsval id, jsval *vp);

static JSBool
obj_setSlot(JSContext *cx, JSObject *obj, jsval id, jsval *vp);

static JSBool
obj_getCount(JSContext *cx, JSObject *obj, jsval id, jsval *vp);

static JSPropertySpec object_props[] = {
    /* These two must come first; see object_props[slot].name usage below. */
    {js_proto_str, JSSLOT_PROTO, JSPROP_PERMANENT|JSPROP_SHARED,
                                                  obj_getSlot,  obj_setSlot},
    {js_parent_str,JSSLOT_PARENT,JSPROP_READONLY|JSPROP_PERMANENT|JSPROP_SHARED,
                                                  obj_getSlot,  obj_setSlot},
    {js_count_str, 0,            JSPROP_PERMANENT,obj_getCount, obj_getCount},
    {0,0,0,0,0}
};

/* NB: JSSLOT_PROTO and JSSLOT_PARENT are already indexes into object_props. */
#define JSSLOT_COUNT 2

static JSBool
ReportStrictSlot(JSContext *cx, uint32 slot)
{
    if (slot == JSSLOT_PROTO)
        return JS_TRUE;
    return JS_ReportErrorFlagsAndNumber(cx,
                                        JSREPORT_WARNING | JSREPORT_STRICT,
                                        js_GetErrorMessage, NULL,
                                        JSMSG_DEPRECATED_USAGE,
                                        object_props[slot].name);
}

static JSBool
obj_getSlot(JSContext *cx, JSObject *obj, jsval id, jsval *vp)
{
    uint32 slot;
    jsid propid;
    JSAccessMode mode;
    uintN attrs;
    JSObject *pobj;
    JSClass *clasp;
    JSExtendedClass *xclasp;

    slot = (uint32) JSVAL_TO_INT(id);
    if (id == INT_TO_JSVAL(JSSLOT_PROTO)) {
        propid = ATOM_TO_JSID(cx->runtime->atomState.protoAtom);
        mode = JSACC_PROTO;
    } else {
        propid = ATOM_TO_JSID(cx->runtime->atomState.parentAtom);
        mode = JSACC_PARENT;
    }

    /* Let OBJ_CHECK_ACCESS get the slot's value, based on the access mode. */
    if (!OBJ_CHECK_ACCESS(cx, obj, propid, mode, vp, &attrs))
        return JS_FALSE;

    pobj = JSVAL_TO_OBJECT(*vp);
    if (pobj) {
        clasp = OBJ_GET_CLASS(cx, pobj);
        if (clasp == &js_CallClass || clasp == &js_BlockClass) {
            /* Censor activations and lexical scopes per ECMA-262. */
            *vp = JSVAL_NULL;
        } else if (clasp->flags & JSCLASS_IS_EXTENDED) {
            xclasp = (JSExtendedClass *) clasp;
            if (xclasp->outerObject) {
                pobj = xclasp->outerObject(cx, pobj);
                if (!pobj)
                    return JS_FALSE;
                *vp = OBJECT_TO_JSVAL(pobj);
            }
        }
    }
    return JS_TRUE;
}

static JSBool
obj_setSlot(JSContext *cx, JSObject *obj, jsval id, jsval *vp)
{
    JSObject *pobj;
    uint32 slot;
    jsid propid;
    uintN attrs;

    if (!JSVAL_IS_OBJECT(*vp))
        return JS_TRUE;
    pobj = JSVAL_TO_OBJECT(*vp);

    if (pobj) {
        /*
         * Innerize pobj here to avoid sticking unwanted properties on the
         * outer object. This ensures that any with statements only grant
         * access to the inner object.
         */
        OBJ_TO_INNER_OBJECT(cx, pobj);
        if (!pobj)
            return JS_FALSE;
    }
    slot = (uint32) JSVAL_TO_INT(id);
    if (JS_HAS_STRICT_OPTION(cx) && !ReportStrictSlot(cx, slot))
        return JS_FALSE;

    /* __parent__ is readonly and permanent, only __proto__ may be set. */
    propid = ATOM_TO_JSID(cx->runtime->atomState.protoAtom);
    if (!OBJ_CHECK_ACCESS(cx, obj, propid, JSACC_PROTO|JSACC_WRITE, vp, &attrs))
        return JS_FALSE;

    return js_SetProtoOrParent(cx, obj, slot, pobj);
}

static JSBool
obj_getCount(JSContext *cx, JSObject *obj, jsval id, jsval *vp)
{
    jsval iter_state;
    jsid num_properties;
    JSBool ok;

    if (JS_HAS_STRICT_OPTION(cx) && !ReportStrictSlot(cx, JSSLOT_COUNT))
        return JS_FALSE;

    /* Get the number of properties to enumerate. */
    iter_state = JSVAL_NULL;
    ok = OBJ_ENUMERATE(cx, obj, JSENUMERATE_INIT, &iter_state, &num_properties);
    if (!ok)
        goto out;

    if (!JSVAL_IS_INT(num_properties)) {
        JS_ASSERT(0);
        *vp = JSVAL_ZERO;
        goto out;
    }
    *vp = num_properties;

out:
    if (iter_state != JSVAL_NULL)
        ok = OBJ_ENUMERATE(cx, obj, JSENUMERATE_DESTROY, &iter_state, 0);
    return ok;
}

#else  /* !JS_HAS_OBJ_PROTO_PROP */

#define object_props NULL

#endif /* !JS_HAS_OBJ_PROTO_PROP */

JSBool
js_SetProtoOrParent(JSContext *cx, JSObject *obj, uint32 slot, JSObject *pobj)
{
    JSRuntime *rt;
    JSObject *obj2, *oldproto;
    JSScope *scope, *newscope;

    /*
     * Serialize all proto and parent setting in order to detect cycles.
     * We nest locks in this function, and only here, in the following orders:
     *
     * (1)  rt->setSlotLock < pobj's scope lock;
     *      rt->setSlotLock < pobj's proto-or-parent's scope lock;
     *      rt->setSlotLock < pobj's grand-proto-or-parent's scope lock;
     *      etc...
     * (2)  rt->setSlotLock < obj's scope lock < pobj's scope lock.
     *
     * We avoid AB-BA deadlock by restricting obj from being on pobj's parent
     * or proto chain (pobj may already be on obj's parent or proto chain; it
     * could be moving up or down).  We finally order obj with respect to pobj
     * at the bottom of this routine (just before releasing rt->setSlotLock),
     * by making pobj be obj's prototype or parent.
     *
     * After we have set the slot and released rt->setSlotLock, another call
     * to js_SetProtoOrParent could nest locks according to the first order
     * list above, but it cannot deadlock with any other thread.  For there
     * to be a deadlock, other parts of the engine would have to nest scope
     * locks in the opposite order.  XXXbe ensure they don't!
     */
    rt = cx->runtime;
#ifdef JS_THREADSAFE

    JS_ACQUIRE_LOCK(rt->setSlotLock);
    while (rt->setSlotBusy) {
        jsrefcount saveDepth;

        /* Take pains to avoid nesting rt->gcLock inside rt->setSlotLock! */
        JS_RELEASE_LOCK(rt->setSlotLock);
        saveDepth = JS_SuspendRequest(cx);
        JS_ACQUIRE_LOCK(rt->setSlotLock);
        if (rt->setSlotBusy)
            JS_WAIT_CONDVAR(rt->setSlotDone, JS_NO_TIMEOUT);
        JS_RELEASE_LOCK(rt->setSlotLock);
        JS_ResumeRequest(cx, saveDepth);
        JS_ACQUIRE_LOCK(rt->setSlotLock);
    }
    rt->setSlotBusy = JS_TRUE;
    JS_RELEASE_LOCK(rt->setSlotLock);

#define SET_SLOT_DONE(rt)                                                     \
    JS_BEGIN_MACRO                                                            \
        JS_ACQUIRE_LOCK((rt)->setSlotLock);                                   \
        (rt)->setSlotBusy = JS_FALSE;                                         \
        JS_NOTIFY_ALL_CONDVAR((rt)->setSlotDone);                             \
        JS_RELEASE_LOCK((rt)->setSlotLock);                                   \
    JS_END_MACRO

#else

#define SET_SLOT_DONE(rt)       /* nothing */

#endif

    obj2 = pobj;
    while (obj2) {
        if (obj2 == obj) {
            SET_SLOT_DONE(rt);
            JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL,
                                 JSMSG_CYCLIC_VALUE,
#if JS_HAS_OBJ_PROTO_PROP
                                 object_props[slot].name
#else
                                 (slot == JSSLOT_PROTO) ? js_proto_str
                                                        : js_parent_str
#endif
                                 );
            return JS_FALSE;
        }
        obj2 = JSVAL_TO_OBJECT(OBJ_GET_SLOT(cx, obj2, slot));
    }

    if (slot == JSSLOT_PROTO && OBJ_IS_NATIVE(obj)) {
        /* Check to see whether obj shares its prototype's scope. */
        JS_LOCK_OBJ(cx, obj);
        scope = OBJ_SCOPE(obj);
        oldproto = JSVAL_TO_OBJECT(LOCKED_OBJ_GET_SLOT(obj, JSSLOT_PROTO));
        if (oldproto && OBJ_SCOPE(oldproto) == scope) {
            /* Either obj needs a new empty scope, or it should share pobj's. */
            if (!pobj ||
                !OBJ_IS_NATIVE(pobj) ||
                OBJ_GET_CLASS(cx, pobj) != LOCKED_OBJ_GET_CLASS(oldproto)) {
                /*
                 * With no proto and no scope of its own, obj is truly empty.
                 *
                 * If pobj is not native, obj needs its own empty scope -- it
                 * should not continue to share oldproto's scope once oldproto
                 * is not on obj's prototype chain.  That would put properties
                 * from oldproto's scope ahead of properties defined by pobj,
                 * in lookup order.
                 *
                 * If pobj's class differs from oldproto's, we may need a new
                 * scope to handle differences in private and reserved slots,
                 * so we suboptimally but safely make one.
                 */
                scope = js_GetMutableScope(cx, obj);
                if (!scope) {
                    JS_UNLOCK_OBJ(cx, obj);
                    SET_SLOT_DONE(rt);
                    return JS_FALSE;
                }
            } else if (OBJ_SCOPE(pobj) != scope) {
#ifdef JS_THREADSAFE
                /*
                 * We are about to nest scope locks.  Help jslock.c:ShareScope
                 * keep scope->u.count balanced for the JS_UNLOCK_SCOPE, while
                 * avoiding deadlock, by recording scope in rt->setSlotScope.
                 */
                if (scope->ownercx) {
                    JS_ASSERT(scope->ownercx == cx);
                    rt->setSlotScope = scope;
                }
#endif

                /* We can't deadlock because we checked for cycles above (2). */
                JS_LOCK_OBJ(cx, pobj);
                newscope = (JSScope *) js_HoldObjectMap(cx, pobj->map);
                obj->map = &newscope->map;
                js_DropObjectMap(cx, &scope->map, obj);
                JS_TRANSFER_SCOPE_LOCK(cx, scope, newscope);
                scope = newscope;
#ifdef JS_THREADSAFE
                rt->setSlotScope = NULL;
#endif
            }
        }
        LOCKED_OBJ_SET_SLOT(obj, JSSLOT_PROTO, OBJECT_TO_JSVAL(pobj));
        JS_UNLOCK_SCOPE(cx, scope);
    } else {
        OBJ_SET_SLOT(cx, obj, slot, OBJECT_TO_JSVAL(pobj));
    }

    SET_SLOT_DONE(rt);
    return JS_TRUE;

#undef SET_SLOT_DONE
}

JS_STATIC_DLL_CALLBACK(JSHashNumber)
js_hash_object(const void *key)
{
    return (JSHashNumber)JS_PTR_TO_UINT32(key) >> JSVAL_TAGBITS;
}

static JSHashEntry *
MarkSharpObjects(JSContext *cx, JSObject *obj, JSIdArray **idap)
{
    JSSharpObjectMap *map;
    JSHashTable *table;
    JSHashNumber hash;
    JSHashEntry **hep, *he;
    jsatomid sharpid;
    JSIdArray *ida;
    JSBool ok;
    jsint i, length;
    jsid id;
#if JS_HAS_GETTER_SETTER
    JSObject *obj2;
    JSProperty *prop;
    uintN attrs;
#endif
    jsval val;
    int stackDummy;

    if (!JS_CHECK_STACK_SIZE(cx, stackDummy)) {
        JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_OVER_RECURSED);
        return NULL;
    }

    map = &cx->sharpObjectMap;
    table = map->table;
    hash = js_hash_object(obj);
    hep = JS_HashTableRawLookup(table, hash, obj);
    he = *hep;
    if (!he) {
        sharpid = 0;
        he = JS_HashTableRawAdd(table, hep, hash, obj,
                                JS_UINT32_TO_PTR(sharpid));
        if (!he) {
            JS_ReportOutOfMemory(cx);
            return NULL;
        }

        /*
         * Increment map->depth to protect js_EnterSharpObject from reentering
         * itself badly.  Without this fix, if we reenter the basis case where
         * map->depth == 0, when unwinding the inner call we will destroy the
         * newly-created hash table and crash.
         */
        ++map->depth;
        ida = JS_Enumerate(cx, obj);
        --map->depth;
        if (!ida)
            return NULL;

        ok = JS_TRUE;
        for (i = 0, length = ida->length; i < length; i++) {
            id = ida->vector[i];
#if JS_HAS_GETTER_SETTER
            ok = OBJ_LOOKUP_PROPERTY(cx, obj, id, &obj2, &prop);
            if (!ok)
                break;
            if (!prop)
                continue;
            ok = OBJ_GET_ATTRIBUTES(cx, obj2, id, prop, &attrs);
            if (ok) {
                if (OBJ_IS_NATIVE(obj2) &&
                    (attrs & (JSPROP_GETTER | JSPROP_SETTER))) {
                    val = JSVAL_NULL;
                    if (attrs & JSPROP_GETTER)
                        val = (jsval) ((JSScopeProperty*)prop)->getter;
                    if (attrs & JSPROP_SETTER) {
                        if (val != JSVAL_NULL) {
                            /* Mark the getter, then set val to setter. */
                            ok = (MarkSharpObjects(cx, JSVAL_TO_OBJECT(val),
                                                   NULL)
                                  != NULL);
                        }
                        val = (jsval) ((JSScopeProperty*)prop)->setter;
                    }
                } else {
                    ok = OBJ_GET_PROPERTY(cx, obj, id, &val);
                }
            }
            OBJ_DROP_PROPERTY(cx, obj2, prop);
#else
            ok = OBJ_GET_PROPERTY(cx, obj, id, &val);
#endif
            if (!ok)
                break;
            if (!JSVAL_IS_PRIMITIVE(val) &&
                !MarkSharpObjects(cx, JSVAL_TO_OBJECT(val), NULL)) {
                ok = JS_FALSE;
                break;
            }
        }
        if (!ok || !idap)
            JS_DestroyIdArray(cx, ida);
        if (!ok)
            return NULL;
    } else {
        sharpid = JS_PTR_TO_UINT32(he->value);
        if (sharpid == 0) {
            sharpid = ++map->sharpgen << SHARP_ID_SHIFT;
            he->value = JS_UINT32_TO_PTR(sharpid);
        }
        ida = NULL;
    }
    if (idap)
        *idap = ida;
    return he;
}

JSHashEntry *
js_EnterSharpObject(JSContext *cx, JSObject *obj, JSIdArray **idap,
                    jschar **sp)
{
    JSSharpObjectMap *map;
    JSHashTable *table;
    JSIdArray *ida;
    JSHashNumber hash;
    JSHashEntry *he, **hep;
    jsatomid sharpid;
    char buf[20];
    size_t len;

    if (JS_HAS_NATIVE_BRANCH_CALLBACK_OPTION(cx) &&
        cx->branchCallback &&
        !cx->branchCallback(cx, NULL)) {
        return NULL;
    }

    /* Set to null in case we return an early error. */
    *sp = NULL;
    map = &cx->sharpObjectMap;
    table = map->table;
    if (!table) {
        table = JS_NewHashTable(8, js_hash_object, JS_CompareValues,
                                JS_CompareValues, NULL, NULL);
        if (!table) {
            JS_ReportOutOfMemory(cx);
            return NULL;
        }
        map->table = table;
        JS_KEEP_ATOMS(cx->runtime);
    }

    /* From this point the control must flow either through out: or bad:. */
    ida = NULL;
    if (map->depth == 0) {
        he = MarkSharpObjects(cx, obj, &ida);
        if (!he)
            goto bad;
        JS_ASSERT((JS_PTR_TO_UINT32(he->value) & SHARP_BIT) == 0);
        if (!idap) {
            JS_DestroyIdArray(cx, ida);
            ida = NULL;
        }
    } else {
        hash = js_hash_object(obj);
        hep = JS_HashTableRawLookup(table, hash, obj);
        he = *hep;

        /*
         * It's possible that the value of a property has changed from the
         * first time the object's properties are traversed (when the property
         * ids are entered into the hash table) to the second (when they are
         * converted to strings), i.e., the OBJ_GET_PROPERTY() call is not
         * idempotent.
         */
        if (!he) {
            he = JS_HashTableRawAdd(table, hep, hash, obj, NULL);
            if (!he) {
                JS_ReportOutOfMemory(cx);
                goto bad;
            }
            sharpid = 0;
            goto out;
        }
    }

    sharpid = JS_PTR_TO_UINT32(he->value);
    if (sharpid != 0) {
        len = JS_snprintf(buf, sizeof buf, "#%u%c",
                          sharpid >> SHARP_ID_SHIFT,
                          (sharpid & SHARP_BIT) ? '#' : '=');
        *sp = js_InflateString(cx, buf, &len);
        if (!*sp) {
            if (ida)
                JS_DestroyIdArray(cx, ida);
            goto bad;
        }
    }

out:
    JS_ASSERT(he);
    if ((sharpid & SHARP_BIT) == 0) {
        if (idap && !ida) {
            ida = JS_Enumerate(cx, obj);
            if (!ida) {
                if (*sp) {
                    JS_free(cx, *sp);
                    *sp = NULL;
                }
                goto bad;
            }
        }
        map->depth++;
    }

    if (idap)
        *idap = ida;
    return he;

bad:
    /* Clean up the sharpObjectMap table on outermost error. */
    if (map->depth == 0) {
        JS_UNKEEP_ATOMS(cx->runtime);
        map->sharpgen = 0;
        JS_HashTableDestroy(map->table);
        map->table = NULL;
    }
    return NULL;
}

void
js_LeaveSharpObject(JSContext *cx, JSIdArray **idap)
{
    JSSharpObjectMap *map;
    JSIdArray *ida;

    map = &cx->sharpObjectMap;
    JS_ASSERT(map->depth > 0);
    if (--map->depth == 0) {
        JS_UNKEEP_ATOMS(cx->runtime);
        map->sharpgen = 0;
        JS_HashTableDestroy(map->table);
        map->table = NULL;
    }
    if (idap) {
        ida = *idap;
        if (ida) {
            JS_DestroyIdArray(cx, ida);
            *idap = NULL;
        }
    }
}

JS_STATIC_DLL_CALLBACK(intN)
gc_sharp_table_entry_marker(JSHashEntry *he, intN i, void *arg)
{
    GC_MARK((JSContext *)arg, (JSObject *)he->key, "sharp table entry");
    return JS_DHASH_NEXT;
}

void
js_GCMarkSharpMap(JSContext *cx, JSSharpObjectMap *map)
{
    JS_ASSERT(map->depth > 0);
    JS_ASSERT(map->table);

    /*
     * During recursive calls to MarkSharpObjects a non-native object or
     * object with a custom getProperty method can potentially return an
     * unrooted value or even cut from the object graph an argument of one of
     * MarkSharpObjects recursive invocations. So we must protect map->table
     * entries against GC.
     *
     * We can not simply use JSTempValueRooter to mark the obj argument of
     * MarkSharpObjects during recursion as we have to protect *all* entries
     * in JSSharpObjectMap including those that contains otherwise unreachable
     * objects just allocated through custom getProperty. Otherwise newer
     * allocations can re-use the address of an object stored in the hashtable
     * confusing js_EnterSharpObject. So to address the problem we simply
     * mark all objects from map->table.
     *
     * An alternative "proper" solution is to use JSTempValueRooter in
     * MarkSharpObjects with code to remove during finalization entries
     * with otherwise unreachable objects. But this is way too complex
     * to justify spending efforts.
     */
    JS_HashTableEnumerateEntries(map->table, gc_sharp_table_entry_marker, cx);
}

#define OBJ_TOSTRING_EXTRA      4       /* for 4 local GC roots */

#if JS_HAS_TOSOURCE
JSBool
js_obj_toSource(JSContext *cx, JSObject *obj, uintN argc, jsval *argv,
                jsval *rval)
{
    JSBool ok, outermost;
    JSHashEntry *he;
    JSIdArray *ida;
    jschar *chars, *ochars, *vsharp;
    const jschar *idstrchars, *vchars;
    size_t nchars, idstrlength, gsoplength, vlength, vsharplength, curlen;
    char *comma;
    jsint i, j, length, valcnt;
    jsid id;
#if JS_HAS_GETTER_SETTER
    JSObject *obj2;
    JSProperty *prop;
    uintN attrs;
#endif
    jsval *val;
    JSString *gsopold[2];
    JSString *gsop[2];
    JSAtom *atom;
    JSString *idstr, *valstr, *str;
    int stackDummy;

    if (!JS_CHECK_STACK_SIZE(cx, stackDummy)) {
        JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_OVER_RECURSED);
        return JS_FALSE;
    }

    /* If outermost, we need parentheses to be an expression, not a block. */
    outermost = (cx->sharpObjectMap.depth == 0);
    he = js_EnterSharpObject(cx, obj, &ida, &chars);
    if (!he)
        return JS_FALSE;
    if (IS_SHARP(he)) {
        /*
         * We didn't enter -- obj is already "sharp", meaning we've visited it
         * already in our depth first search, and therefore chars contains a
         * string of the form "#n#".
         */
        JS_ASSERT(!ida);
#if JS_HAS_SHARP_VARS
        nchars = js_strlen(chars);
#else
        chars[0] = '{';
        chars[1] = '}';
        chars[2] = 0;
        nchars = 2;
#endif
        goto make_string;
    }
    JS_ASSERT(ida);
    ok = JS_TRUE;

    if (!chars) {
        /* If outermost, allocate 4 + 1 for "({})" and the terminator. */
        chars = (jschar *) malloc(((outermost ? 4 : 2) + 1) * sizeof(jschar));
        nchars = 0;
        if (!chars)
            goto error;
        if (outermost)
            chars[nchars++] = '(';
    } else {
        /* js_EnterSharpObject returned a string of the form "#n=" in chars. */
        MAKE_SHARP(he);
        nchars = js_strlen(chars);
        chars = (jschar *)
            realloc((ochars = chars), (nchars + 2 + 1) * sizeof(jschar));
        if (!chars) {
            free(ochars);
            goto error;
        }
        if (outermost) {
            /*
             * No need for parentheses around the whole shebang, because #n=
             * unambiguously begins an object initializer, and never a block
             * statement.
             */
            outermost = JS_FALSE;
        }
    }

#ifdef DUMP_CALL_TABLE
    if (cx->options & JSOPTION_LOGCALL_TOSOURCE) {
        const char *classname = OBJ_GET_CLASS(cx, obj)->name;
        size_t classnchars = strlen(classname);
        static const char classpropid[] = "C";
        const char *cp;
        size_t onchars = nchars;

        /* 2 for ': ', 2 quotes around classname, 2 for ', ' after. */
        classnchars += sizeof classpropid - 1 + 2 + 2;
        if (ida->length)
            classnchars += 2;

        /* 2 for the braces, 1 for the terminator */
        chars = (jschar *)
            realloc((ochars = chars),
                    (nchars + classnchars + 2 + 1) * sizeof(jschar));
        if (!chars) {
            free(ochars);
            goto error;
        }

        chars[nchars++] = '{';          /* 1 from the 2 braces */
        for (cp = classpropid; *cp; cp++)
            chars[nchars++] = (jschar) *cp;
        chars[nchars++] = ':';
        chars[nchars++] = ' ';          /* 2 for ': ' */
        chars[nchars++] = '"';
        for (cp = classname; *cp; cp++)
            chars[nchars++] = (jschar) *cp;
        chars[nchars++] = '"';          /* 2 quotes */
        if (ida->length) {
            chars[nchars++] = ',';
            chars[nchars++] = ' ';      /* 2 for ', ' */
        }

        JS_ASSERT(nchars - onchars == 1 + classnchars);
    } else
#endif
    chars[nchars++] = '{';

    comma = NULL;

    /*
     * We have four local roots for cooked and raw value GC safety.  Hoist the
     * "argv + 2" out of the loop using the val local, which refers to the raw
     * (unconverted, "uncooked") values.
     */
    val = argv + 2;

    for (i = 0, length = ida->length; i < length; i++) {
        JSBool idIsLexicalIdentifier, needOldStyleGetterSetter;

        /* Get strings for id and value and GC-root them via argv. */
        id = ida->vector[i];

#if JS_HAS_GETTER_SETTER
        ok = OBJ_LOOKUP_PROPERTY(cx, obj, id, &obj2, &prop);
        if (!ok)
            goto error;
#endif

        /*
         * Convert id to a jsval and then to a string.  Decide early whether we
         * prefer get/set or old getter/setter syntax.
         */
        atom = JSID_IS_ATOM(id) ? JSID_TO_ATOM(id) : NULL;
        idstr = js_ValueToString(cx, ID_TO_VALUE(id));
        if (!idstr) {
            ok = JS_FALSE;
            OBJ_DROP_PROPERTY(cx, obj2, prop);
            goto error;
        }
        *rval = STRING_TO_JSVAL(idstr);         /* local root */
        idIsLexicalIdentifier = js_IsIdentifier(idstr);
        needOldStyleGetterSetter =
            !idIsLexicalIdentifier ||
            js_CheckKeyword(JSSTRING_CHARS(idstr),
                            JSSTRING_LENGTH(idstr)) != TOK_EOF;

#if JS_HAS_GETTER_SETTER

        valcnt = 0;
        if (prop) {
            ok = OBJ_GET_ATTRIBUTES(cx, obj2, id, prop, &attrs);
            if (!ok) {
                OBJ_DROP_PROPERTY(cx, obj2, prop);
                goto error;
            }
            if (OBJ_IS_NATIVE(obj2) &&
                (attrs & (JSPROP_GETTER | JSPROP_SETTER))) {
                if (attrs & JSPROP_GETTER) {
                    val[valcnt] = (jsval) ((JSScopeProperty *)prop)->getter;
                    gsopold[valcnt] =
                        ATOM_TO_STRING(cx->runtime->atomState.getterAtom);
                    gsop[valcnt] =
                        ATOM_TO_STRING(cx->runtime->atomState.getAtom);
                    valcnt++;
                }
                if (attrs & JSPROP_SETTER) {
                    val[valcnt] = (jsval) ((JSScopeProperty *)prop)->setter;
                    gsopold[valcnt] =
                        ATOM_TO_STRING(cx->runtime->atomState.setterAtom);
                    gsop[valcnt] =
                        ATOM_TO_STRING(cx->runtime->atomState.setAtom);
                    valcnt++;
                }
            } else {
                valcnt = 1;
                gsop[0] = NULL;
                gsopold[0] = NULL;
                ok = OBJ_GET_PROPERTY(cx, obj, id, &val[0]);
            }
            OBJ_DROP_PROPERTY(cx, obj2, prop);
        }

#else  /* !JS_HAS_GETTER_SETTER */

        /*
         * We simplify the source code at the price of minor dead code bloat in
         * the ECMA version (for testing only, see jsconfig.h).  The null
         * default values in gsop[j] suffice to disable non-ECMA getter and
         * setter code.
         */
        valcnt = 1;
        gsop[0] = NULL;
        gsopold[0] = NULL;
        ok = OBJ_GET_PROPERTY(cx, obj, id, &val[0]);

#endif /* !JS_HAS_GETTER_SETTER */

        if (!ok)
            goto error;

        /*
         * If id is a string that's not an identifier, then it needs to be
         * quoted.  Also, negative integer ids must be quoted.
         */
        if (atom
            ? !idIsLexicalIdentifier
            : (JSID_IS_OBJECT(id) || JSID_TO_INT(id) < 0)) {
            idstr = js_QuoteString(cx, idstr, (jschar)'\'');
            if (!idstr) {
                ok = JS_FALSE;
                goto error;
            }
            *rval = STRING_TO_JSVAL(idstr);     /* local root */
        }
        idstrchars = JSSTRING_CHARS(idstr);
        idstrlength = JSSTRING_LENGTH(idstr);

        for (j = 0; j < valcnt; j++) {
            /* Convert val[j] to its canonical source form. */
            valstr = js_ValueToSource(cx, val[j]);
            if (!valstr) {
                ok = JS_FALSE;
                goto error;
            }
            argv[j] = STRING_TO_JSVAL(valstr);  /* local root */
            vchars = JSSTRING_CHARS(valstr);
            vlength = JSSTRING_LENGTH(valstr);

            if (vchars[0] == '#')
                needOldStyleGetterSetter = JS_TRUE;

            if (needOldStyleGetterSetter)
                gsop[j] = gsopold[j];

#ifndef OLD_GETTER_SETTER
            /*
             * Remove '(function ' from the beginning of valstr and ')' from the
             * end so that we can put "get" in front of the function definition.
             */
            if (gsop[j] && VALUE_IS_FUNCTION(cx, val[j]) &&
                !needOldStyleGetterSetter) {
                const jschar *start = vchars;
                if (vchars[0] == '(')
                    vchars++;
                vchars = js_strchr_limit(vchars, '(', vchars + vlength);
                if (vchars) {
                    vlength -= vchars - start + 1;
                } else {
                    gsop[j] = NULL;
                    vchars = start;
                }
            }
#else
            needOldStyleGetterSetter = JS_TRUE;
            gsop[j] = gsopold[j];
#endif

            /* If val[j] is a non-sharp object, consider sharpening it. */
            vsharp = NULL;
            vsharplength = 0;
#if JS_HAS_SHARP_VARS
            if (!JSVAL_IS_PRIMITIVE(val[j]) && vchars[0] != '#') {
                he = js_EnterSharpObject(cx, JSVAL_TO_OBJECT(val[j]), NULL,
                                         &vsharp);
                if (!he) {
                    ok = JS_FALSE;
                    goto error;
                }
                if (IS_SHARP(he)) {
                    vchars = vsharp;
                    vlength = js_strlen(vchars);
                    needOldStyleGetterSetter = JS_TRUE;
                    gsop[j] = gsopold[j];
                } else {
                    if (vsharp) {
                        vsharplength = js_strlen(vsharp);
                        MAKE_SHARP(he);
                        needOldStyleGetterSetter = JS_TRUE;
                        gsop[j] = gsopold[j];
                    }
                    js_LeaveSharpObject(cx, NULL);
                }
            }
#endif

#define SAFE_ADD(n)                                                          \
    JS_BEGIN_MACRO                                                           \
        size_t n_ = (n);                                                     \
        curlen += n_;                                                        \
        if (curlen < n_)                                                     \
            goto overflow;                                                   \
    JS_END_MACRO

            curlen = nchars;
            if (comma)
                SAFE_ADD(2);
            SAFE_ADD(idstrlength + 1);
            if (gsop[j])
                SAFE_ADD(JSSTRING_LENGTH(gsop[j]) + 1);
            SAFE_ADD(vsharplength);
            SAFE_ADD(vlength);
            /* Account for the trailing null. */
            SAFE_ADD((outermost ? 2 : 1) + 1);
#undef SAFE_ADD

            if (curlen > (size_t)-1 / sizeof(jschar))
                goto overflow;

            /* Allocate 1 + 1 at end for closing brace and terminating 0. */
            chars = (jschar *)
                realloc((ochars = chars), curlen * sizeof(jschar));
            if (!chars) {
                /* Save code space on error: let JS_free ignore null vsharp. */
                JS_free(cx, vsharp);
                free(ochars);
                goto error;
            }

            if (comma) {
                chars[nchars++] = comma[0];
                chars[nchars++] = comma[1];
            }
            comma = ", ";

            if (needOldStyleGetterSetter) {
                js_strncpy(&chars[nchars], idstrchars, idstrlength);
                nchars += idstrlength;
                if (gsop[j]) {
                    chars[nchars++] = ' ';
                    gsoplength = JSSTRING_LENGTH(gsop[j]);
                    js_strncpy(&chars[nchars], JSSTRING_CHARS(gsop[j]),
                               gsoplength);
                    nchars += gsoplength;
                }
                chars[nchars++] = ':';
            } else {  /* New style "decompilation" */
                if (gsop[j]) {
                    gsoplength = JSSTRING_LENGTH(gsop[j]);
                    js_strncpy(&chars[nchars], JSSTRING_CHARS(gsop[j]),
                               gsoplength);
                    nchars += gsoplength;
                    chars[nchars++] = ' ';
                }
                js_strncpy(&chars[nchars], idstrchars, idstrlength);
                nchars += idstrlength;
                /* Extraneous space after id here will be extracted later */
                chars[nchars++] = gsop[j] ? ' ' : ':';
            }

            if (vsharplength) {
                js_strncpy(&chars[nchars], vsharp, vsharplength);
                nchars += vsharplength;
            }
            js_strncpy(&chars[nchars], vchars, vlength);
            nchars += vlength;

            if (vsharp)
                JS_free(cx, vsharp);
#ifdef DUMP_CALL_TABLE
            if (outermost && nchars >= js_LogCallToSourceLimit)
                break;
#endif
        }
    }

    chars[nchars++] = '}';
    if (outermost)
        chars[nchars++] = ')';
    chars[nchars] = 0;

  error:
    js_LeaveSharpObject(cx, &ida);

    if (!ok) {
        if (chars)
            free(chars);
        return ok;
    }

    if (!chars) {
        JS_ReportOutOfMemory(cx);
        return JS_FALSE;
    }
  make_string:
    str = js_NewString(cx, chars, nchars, 0);
    if (!str) {
        free(chars);
        return JS_FALSE;
    }
    *rval = STRING_TO_JSVAL(str);
    return JS_TRUE;

  overflow:
    JS_free(cx, vsharp);
    free(chars);
    chars = NULL;
    goto error;
}
#endif /* JS_HAS_TOSOURCE */

JSBool
js_obj_toString(JSContext *cx, JSObject *obj, uintN argc, jsval *argv,
                jsval *rval)
{
    jschar *chars;
    size_t nchars;
    const char *clazz, *prefix;
    JSString *str;

    clazz = OBJ_GET_CLASS(cx, obj)->name;
    nchars = 9 + strlen(clazz);         /* 9 for "[object ]" */
    chars = (jschar *) JS_malloc(cx, (nchars + 1) * sizeof(jschar));
    if (!chars)
        return JS_FALSE;

    prefix = "[object ";
    nchars = 0;
    while ((chars[nchars] = (jschar)*prefix) != 0)
        nchars++, prefix++;
    while ((chars[nchars] = (jschar)*clazz) != 0)
        nchars++, clazz++;
    chars[nchars++] = ']';
    chars[nchars] = 0;

    str = js_NewString(cx, chars, nchars, 0);
    if (!str) {
        JS_free(cx, chars);
        return JS_FALSE;
    }
    *rval = STRING_TO_JSVAL(str);
    return JS_TRUE;
}

static JSBool
js_obj_toLocaleString(JSContext *cx, JSObject *obj, uintN argc, jsval *argv,
                      jsval *rval)
{
    JSString *str;

    str = js_ValueToString(cx, argv[-1]);
    if (!str)
        return JS_FALSE;

    *rval = STRING_TO_JSVAL(str);
    return JS_TRUE;
}

static JSBool
obj_valueOf(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval)
{
    *rval = OBJECT_TO_JSVAL(obj);
    return JS_TRUE;
}

/*
 * Check whether principals subsumes scopeobj's principals, and return true
 * if so (or if scopeobj has no principals, for backward compatibility with
 * the JS API, which does not require principals), and false otherwise.
 */
JSBool
js_CheckPrincipalsAccess(JSContext *cx, JSObject *scopeobj,
                         JSPrincipals *principals, JSAtom *caller)
{
    JSRuntime *rt;
    JSPrincipals *scopePrincipals;
    const char *callerstr;

    rt = cx->runtime;
    if (rt->findObjectPrincipals) {
        scopePrincipals = rt->findObjectPrincipals(cx, scopeobj);
        if (!principals || !scopePrincipals ||
            !principals->subsume(principals, scopePrincipals)) {
            callerstr = js_AtomToPrintableString(cx, caller);
            if (!callerstr)
                return JS_FALSE;
            JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL,
                                 JSMSG_BAD_INDIRECT_CALL, callerstr);
            return JS_FALSE;
        }
    }
    return JS_TRUE;
}

JSObject *
js_CheckScopeChainValidity(JSContext *cx, JSObject *scopeobj, const char *caller)
{
    JSClass *clasp;
    JSExtendedClass *xclasp;
    JSObject *inner;

    if (!scopeobj)
        goto bad;

    OBJ_TO_INNER_OBJECT(cx, scopeobj);
    if (!scopeobj)
        return NULL;

    inner = scopeobj;

    /* XXX This is an awful gross hack. */
    while (scopeobj) {
        clasp = OBJ_GET_CLASS(cx, scopeobj);
        if (clasp->flags & JSCLASS_IS_EXTENDED) {
            xclasp = (JSExtendedClass*)clasp;
            if (xclasp->innerObject &&
                xclasp->innerObject(cx, scopeobj) != scopeobj) {
                goto bad;
            }
        }

        scopeobj = OBJ_GET_PARENT(cx, scopeobj);
    }

    return inner;

bad:
    JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL,
                         JSMSG_BAD_INDIRECT_CALL, caller);
    return NULL;
}

static JSBool
obj_eval(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval)
{
    JSStackFrame *fp, *caller;
    JSBool indirectCall;
    JSObject *scopeobj;
    JSString *str;
    const char *file;
    uintN line;
    JSPrincipals *principals;
    JSScript *script;
    JSBool ok;
#if JS_HAS_EVAL_THIS_SCOPE
    JSObject *callerScopeChain = NULL, *callerVarObj = NULL;
    JSObject *setCallerScopeChain = NULL;
    JSBool setCallerVarObj = JS_FALSE;
#endif

    fp = cx->fp;
    caller = JS_GetScriptedCaller(cx, fp);
    JS_ASSERT(!caller || caller->pc);
    indirectCall = (caller && *caller->pc != JSOP_EVAL);

    if (indirectCall &&
        !JS_ReportErrorFlagsAndNumber(cx,
                                      JSREPORT_WARNING | JSREPORT_STRICT,
                                      js_GetErrorMessage, NULL,
                                      JSMSG_BAD_INDIRECT_CALL,
                                      js_eval_str)) {
        return JS_FALSE;
    }

    if (!JSVAL_IS_STRING(argv[0])) {
        *rval = argv[0];
        return JS_TRUE;
    }

    /*
     * If the caller is a lightweight function and doesn't have a variables
     * object, then we need to provide one for the compiler to stick any
     * declared (var) variables into.
     */
    if (caller && !caller->varobj && !js_GetCallObject(cx, caller, NULL))
        return JS_FALSE;

#if JS_HAS_SCRIPT_OBJECT
    /*
     * Script.prototype.compile/exec and Object.prototype.eval all take an
     * optional trailing argument that overrides the scope object.
     */
    scopeobj = NULL;
    if (argc >= 2) {
        if (!js_ValueToObject(cx, argv[1], &scopeobj))
            return JS_FALSE;
        argv[1] = OBJECT_TO_JSVAL(scopeobj);
    }
    if (!scopeobj)
#endif
    {
#if JS_HAS_EVAL_THIS_SCOPE
        /* If obj.eval(str), emulate 'with (obj) eval(str)' in the caller. */
        if (indirectCall) {
            callerScopeChain = js_GetScopeChain(cx, caller);
            if (!callerScopeChain)
                return JS_FALSE;
            OBJ_TO_INNER_OBJECT(cx, obj);
            if (!obj)
                return JS_FALSE;
            if (obj != callerScopeChain) {
                if (!js_CheckPrincipalsAccess(cx, obj,
                                              caller->script->principals,
                                              cx->runtime->atomState.evalAtom))
                {
                    return JS_FALSE;
                }

                scopeobj = js_NewWithObject(cx, obj, callerScopeChain, -1);
                if (!scopeobj)
                    return JS_FALSE;

                /* Set fp->scopeChain too, for the compiler. */
                caller->scopeChain = fp->scopeChain = scopeobj;

                /* Remember scopeobj so we can null its private when done. */
                setCallerScopeChain = scopeobj;
            }

            callerVarObj = caller->varobj;
            if (obj != callerVarObj) {
                /* Set fp->varobj too, for the compiler. */
                caller->varobj = fp->varobj = obj;
                setCallerVarObj = JS_TRUE;
            }
        }
        /* From here on, control must exit through label out with ok set. */
#endif

        /* Compile using caller's current scope object. */
        if (caller) {
            scopeobj = js_GetScopeChain(cx, caller);
            if (!scopeobj) {
                ok = JS_FALSE;
                goto out;
            }
        }
    }

    /* Ensure we compile this eval with the right object in the scope chain. */
    scopeobj = js_CheckScopeChainValidity(cx, scopeobj, js_eval_str);
    if (!scopeobj)
        return JS_FALSE;

    str = JSVAL_TO_STRING(argv[0]);
    if (caller) {
        principals = JS_EvalFramePrincipals(cx, fp, caller);
        if (principals == caller->script->principals) {
            file = caller->script->filename;
            line = js_PCToLineNumber(cx, caller->script, caller->pc);
        } else {
            file = principals->codebase;
            line = 0;
        }
    } else {
        file = NULL;
        line = 0;
        principals = NULL;
    }

    /*
     * Set JSFRAME_EVAL on fp and any frames (e.g., fun_call if eval.call was
     * invoked) between fp and its scripted caller, to help the compiler easily
     * find the same caller whose scope and var obj we've set.
     *
     * XXX this nonsense could, and perhaps should, go away with a better way
     * to pass params to the compiler than via the top-most frame.
     */
    do {
        fp->flags |= JSFRAME_EVAL;
    } while ((fp = fp->down) != caller);

    script = JS_CompileUCScriptForPrincipals(cx, scopeobj, principals,
                                             JSSTRING_CHARS(str),
                                             JSSTRING_LENGTH(str),
                                             file, line);
    if (!script) {
        ok = JS_FALSE;
        goto out;
    }

#if JS_HAS_SCRIPT_OBJECT
    if (argc < 2)
#endif
    {
        /* Execute using caller's new scope object (might be a Call object). */
        if (caller)
            scopeobj = caller->scopeChain;
    }

    /*
     * Belt-and-braces: check that the lesser of eval's principals and the
     * caller's principals has access to scopeobj.
     */
    ok = js_CheckPrincipalsAccess(cx, scopeobj, principals,
                                  cx->runtime->atomState.evalAtom);
    if (ok)
        ok = js_Execute(cx, scopeobj, script, caller, JSFRAME_EVAL, rval);

    JS_DestroyScript(cx, script);

out:
#if JS_HAS_EVAL_THIS_SCOPE
    /* Restore OBJ_GET_PARENT(scopeobj) not callerScopeChain in case of Call. */
    if (setCallerScopeChain) {
        caller->scopeChain = callerScopeChain;
        JS_ASSERT(OBJ_GET_CLASS(cx, setCallerScopeChain) == &js_WithClass);
        JS_SetPrivate(cx, setCallerScopeChain, NULL);
    }
    if (setCallerVarObj)
        caller->varobj = callerVarObj;
#endif
    return ok;
}

#if JS_HAS_OBJ_WATCHPOINT

static JSBool
obj_watch_handler(JSContext *cx, JSObject *obj, jsval id, jsval old, jsval *nvp,
                  void *closure)
{
    JSObject *callable;
    JSRuntime *rt;
    JSStackFrame *caller;
    JSPrincipals *subject, *watcher;
    JSResolvingKey key;
    JSResolvingEntry *entry;
    uint32 generation;
    jsval argv[3];
    JSBool ok;

    callable = (JSObject *) closure;

    rt = cx->runtime;
    if (rt->findObjectPrincipals) {
        /* Skip over any obj_watch_* frames between us and the real subject. */
        caller = JS_GetScriptedCaller(cx, cx->fp);
        if (caller) {
            /*
             * Only call the watch handler if the watcher is allowed to watch
             * the currently executing script.
             */
            watcher = rt->findObjectPrincipals(cx, callable);
            subject = JS_StackFramePrincipals(cx, caller);

            if (watcher && subject && !watcher->subsume(watcher, subject)) {
                /* Silently don't call the watch handler. */
                return JS_TRUE;
            }
        }
    }

    /* Avoid recursion on (obj, id) already being watched on cx. */
    key.obj = obj;
    key.id = id;
    if (!js_StartResolving(cx, &key, JSRESFLAG_WATCH, &entry))
        return JS_FALSE;
    if (!entry)
        return JS_TRUE;
    generation = cx->resolvingTable->generation;

    argv[0] = id;
    argv[1] = old;
    argv[2] = *nvp;
    ok = js_InternalCall(cx, obj, OBJECT_TO_JSVAL(callable), 3, argv, nvp);
    js_StopResolving(cx, &key, JSRESFLAG_WATCH, entry, generation);
    return ok;
}

static JSBool
obj_watch(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval)
{
    JSObject *callable;
    jsval userid, value;
    jsid propid;
    uintN attrs;

    callable = js_ValueToCallableObject(cx, &argv[1], 0);
    if (!callable)
        return JS_FALSE;

    /* Compute the unique int/atom symbol id needed by js_LookupProperty. */
    userid = argv[0];
    if (!JS_ValueToId(cx, userid, &propid))
        return JS_FALSE;

    if (!OBJ_CHECK_ACCESS(cx, obj, propid, JSACC_WATCH, &value, &attrs))
        return JS_FALSE;
    if (attrs & JSPROP_READONLY)
        return JS_TRUE;
    return JS_SetWatchPoint(cx, obj, userid, obj_watch_handler, callable);
}

static JSBool
obj_unwatch(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval)
{
    return JS_ClearWatchPoint(cx, obj, argv[0], NULL, NULL);
}

#endif /* JS_HAS_OBJ_WATCHPOINT */

/*
 * Prototype and property query methods, to complement the 'in' and
 * 'instanceof' operators.
 */

/* Proposed ECMA 15.2.4.5. */
static JSBool
obj_hasOwnProperty(JSContext *cx, JSObject *obj, uintN argc, jsval *argv,
                   jsval *rval)
{
    return js_HasOwnPropertyHelper(cx, obj, obj->map->ops->lookupProperty,
                                   argc, argv, rval);
}

JSBool
js_HasOwnPropertyHelper(JSContext *cx, JSObject *obj, JSLookupPropOp lookup,
                        uintN argc, jsval *argv, jsval *rval)
{
    jsid id;
    JSObject *obj2;
    JSProperty *prop;
    JSScopeProperty *sprop;

    if (!JS_ValueToId(cx, argv[0], &id))
        return JS_FALSE;
    if (!lookup(cx, obj, id, &obj2, &prop))
        return JS_FALSE;
    if (!prop) {
        *rval = JSVAL_FALSE;
    } else if (obj2 == obj) {
        *rval = JSVAL_TRUE;
    } else {
        JSClass *clasp;
        JSExtendedClass *xclasp;

        clasp = OBJ_GET_CLASS(cx, obj);
        xclasp = (clasp->flags & JSCLASS_IS_EXTENDED)
                 ? (JSExtendedClass *)clasp
                 : NULL;
        if (xclasp && xclasp->outerObject &&
            xclasp->outerObject(cx, obj2) == obj) {
            *rval = JSVAL_TRUE;
        } else if (OBJ_IS_NATIVE(obj2) && OBJ_GET_CLASS(cx, obj2) == clasp) {
            /*
             * The combination of JSPROP_SHARED and JSPROP_PERMANENT in a
             * delegated property makes that property appear to be direct in
             * all delegating instances of the same native class.  This hack
             * avoids bloating every function instance with its own 'length'
             * (AKA 'arity') property.  But it must not extend across class
             * boundaries, to avoid making hasOwnProperty lie (bug 320854).
             *
             * It's not really a hack, of course: a permanent property can't
             * be deleted, and JSPROP_SHARED means "don't allocate a slot in
             * any instance, prototype or delegating".  Without a slot, and
             * without the ability to remove and recreate (with differences)
             * the property, there is no way to tell whether it is directly
             * owned, or indirectly delegated.
             */
            sprop = (JSScopeProperty *)prop;
            *rval = BOOLEAN_TO_JSVAL(SPROP_IS_SHARED_PERMANENT(sprop));
        } else {
            *rval = JSVAL_FALSE;
        }
    }
    if (prop)
        OBJ_DROP_PROPERTY(cx, obj2, prop);
    return JS_TRUE;
}

/* Proposed ECMA 15.2.4.6. */
static JSBool
obj_isPrototypeOf(JSContext *cx, JSObject *obj, uintN argc, jsval *argv,
                  jsval *rval)
{
    JSBool b;

    if (!js_IsDelegate(cx, obj, *argv, &b))
        return JS_FALSE;
    *rval = BOOLEAN_TO_JSVAL(b);
    return JS_TRUE;
}

/* Proposed ECMA 15.2.4.7. */
static JSBool
obj_propertyIsEnumerable(JSContext *cx, JSObject *obj, uintN argc, jsval *argv,
                         jsval *rval)
{
    jsid id;
    uintN attrs;
    JSObject *obj2;
    JSProperty *prop;
    JSBool ok;

    if (!JS_ValueToId(cx, argv[0], &id))
        return JS_FALSE;

    if (!OBJ_LOOKUP_PROPERTY(cx, obj, id, &obj2, &prop))
        return JS_FALSE;

    if (!prop) {
        *rval = JSVAL_FALSE;
        return JS_TRUE;
    }

    /*
     * XXX ECMA spec error compatible: return false unless hasOwnProperty.
     * The ECMA spec really should be fixed so propertyIsEnumerable and the
     * for..in loop agree on whether prototype properties are enumerable,
     * obviously by fixing this method (not by breaking the for..in loop!).
     *
     * We check here for shared permanent prototype properties, which should
     * be treated as if they are local to obj.  They are an implementation
     * technique used to satisfy ECMA requirements; users should not be able
     * to distinguish a shared permanent proto-property from a local one.
     */
    if (obj2 != obj &&
        !(OBJ_IS_NATIVE(obj2) &&
          SPROP_IS_SHARED_PERMANENT((JSScopeProperty *)prop))) {
        OBJ_DROP_PROPERTY(cx, obj2, prop);
        *rval = JSVAL_FALSE;
        return JS_TRUE;
    }

    ok = OBJ_GET_ATTRIBUTES(cx, obj2, id, prop, &attrs);
    OBJ_DROP_PROPERTY(cx, obj2, prop);
    if (ok)
        *rval = BOOLEAN_TO_JSVAL((attrs & JSPROP_ENUMERATE) != 0);
    return ok;
}

#if JS_HAS_GETTER_SETTER
static JSBool
obj_defineGetter(JSContext *cx, JSObject *obj, uintN argc, jsval *argv,
                 jsval *rval)
{
    jsval fval, junk;
    jsid id;
    uintN attrs;

    fval = argv[1];
    if (JS_TypeOfValue(cx, fval) != JSTYPE_FUNCTION) {
        JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL,
                             JSMSG_BAD_GETTER_OR_SETTER,
                             js_getter_str);
        return JS_FALSE;
    }

    if (!JS_ValueToId(cx, argv[0], &id))
        return JS_FALSE;
    if (!js_CheckRedeclaration(cx, obj, id, JSPROP_GETTER, NULL, NULL))
        return JS_FALSE;
    /*
     * Getters and setters are just like watchpoints from an access
     * control point of view.
     */
    if (!OBJ_CHECK_ACCESS(cx, obj, id, JSACC_WATCH, &junk, &attrs))
        return JS_FALSE;
    return OBJ_DEFINE_PROPERTY(cx, obj, id, JSVAL_VOID,
                               (JSPropertyOp) JSVAL_TO_OBJECT(fval), NULL,
                               JSPROP_ENUMERATE | JSPROP_GETTER | JSPROP_SHARED,
                               NULL);
}

static JSBool
obj_defineSetter(JSContext *cx, JSObject *obj, uintN argc, jsval *argv,
                 jsval *rval)
{
    jsval fval, junk;
    jsid id;
    uintN attrs;

    fval = argv[1];
    if (JS_TypeOfValue(cx, fval) != JSTYPE_FUNCTION) {
        JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL,
                             JSMSG_BAD_GETTER_OR_SETTER,
                             js_setter_str);
        return JS_FALSE;
    }

    if (!JS_ValueToId(cx, argv[0], &id))
        return JS_FALSE;
    if (!js_CheckRedeclaration(cx, obj, id, JSPROP_SETTER, NULL, NULL))
        return JS_FALSE;
    /*
     * Getters and setters are just like watchpoints from an access
     * control point of view.
     */
    if (!OBJ_CHECK_ACCESS(cx, obj, id, JSACC_WATCH, &junk, &attrs))
        return JS_FALSE;
    return OBJ_DEFINE_PROPERTY(cx, obj, id, JSVAL_VOID,
                               NULL, (JSPropertyOp) JSVAL_TO_OBJECT(fval),
                               JSPROP_ENUMERATE | JSPROP_SETTER | JSPROP_SHARED,
                               NULL);
}

static JSBool
obj_lookupGetter(JSContext *cx, JSObject *obj, uintN argc, jsval *argv,
                 jsval *rval)
{
    jsid id;
    JSObject *pobj;
    JSProperty *prop;
    JSScopeProperty *sprop;

    if (!JS_ValueToId(cx, argv[0], &id))
        return JS_FALSE;
    if (!OBJ_LOOKUP_PROPERTY(cx, obj, id, &pobj, &prop))
        return JS_FALSE;
    if (prop) {
        if (OBJ_IS_NATIVE(pobj)) {
            sprop = (JSScopeProperty *) prop;
            if (sprop->attrs & JSPROP_GETTER)
                *rval = OBJECT_TO_JSVAL(sprop->getter);
        }
        OBJ_DROP_PROPERTY(cx, pobj, prop);
    }
    return JS_TRUE;
}

static JSBool
obj_lookupSetter(JSContext *cx, JSObject *obj, uintN argc, jsval *argv,
                 jsval *rval)
{
    jsid id;
    JSObject *pobj;
    JSProperty *prop;
    JSScopeProperty *sprop;

    if (!JS_ValueToId(cx, argv[0], &id))
        return JS_FALSE;
    if (!OBJ_LOOKUP_PROPERTY(cx, obj, id, &pobj, &prop))
        return JS_FALSE;
    if (prop) {
        if (OBJ_IS_NATIVE(pobj)) {
            sprop = (JSScopeProperty *) prop;
            if (sprop->attrs & JSPROP_SETTER)
                *rval = OBJECT_TO_JSVAL(sprop->setter);
        }
        OBJ_DROP_PROPERTY(cx, pobj, prop);
    }
    return JS_TRUE;
}
#endif /* JS_HAS_GETTER_SETTER */

#if JS_HAS_OBJ_WATCHPOINT
const char js_watch_str[] = "watch";
const char js_unwatch_str[] = "unwatch";
#endif
const char js_hasOwnProperty_str[] = "hasOwnProperty";
const char js_isPrototypeOf_str[] = "isPrototypeOf";
const char js_propertyIsEnumerable_str[] = "propertyIsEnumerable";
#if JS_HAS_GETTER_SETTER
const char js_defineGetter_str[] = "__defineGetter__";
const char js_defineSetter_str[] = "__defineSetter__";
const char js_lookupGetter_str[] = "__lookupGetter__";
const char js_lookupSetter_str[] = "__lookupSetter__";
#endif

static JSFunctionSpec object_methods[] = {
#if JS_HAS_TOSOURCE
    {js_toSource_str,             js_obj_toSource,    0, 0, OBJ_TOSTRING_EXTRA},
#endif
    {js_toString_str,             js_obj_toString,    0, 0, OBJ_TOSTRING_EXTRA},
    {js_toLocaleString_str,       js_obj_toLocaleString, 0, 0, OBJ_TOSTRING_EXTRA},
    {js_valueOf_str,              obj_valueOf,        0,0,0},
    {js_eval_str,                 obj_eval,           1,0,0},
#if JS_HAS_OBJ_WATCHPOINT
    {js_watch_str,                obj_watch,          2,0,0},
    {js_unwatch_str,              obj_unwatch,        1,0,0},
#endif
    {js_hasOwnProperty_str,       obj_hasOwnProperty, 1,0,0},
    {js_isPrototypeOf_str,        obj_isPrototypeOf,  1,0,0},
    {js_propertyIsEnumerable_str, obj_propertyIsEnumerable, 1,0,0},
#if JS_HAS_GETTER_SETTER
    {js_defineGetter_str,         obj_defineGetter,   2,0,0},
    {js_defineSetter_str,         obj_defineSetter,   2,0,0},
    {js_lookupGetter_str,         obj_lookupGetter,   1,0,0},
    {js_lookupSetter_str,         obj_lookupSetter,   1,0,0},
#endif
    {0,0,0,0,0}
};

static JSBool
Object(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval)
{
    if (argc == 0) {
        /* Trigger logic below to construct a blank object. */
        obj = NULL;
    } else {
        /* If argv[0] is null or undefined, obj comes back null. */
        if (!js_ValueToObject(cx, argv[0], &obj))
            return JS_FALSE;
    }
    if (!obj) {
        JS_ASSERT(!argc || JSVAL_IS_NULL(argv[0]) || JSVAL_IS_VOID(argv[0]));
        if (cx->fp->flags & JSFRAME_CONSTRUCTING)
            return JS_TRUE;
        obj = js_NewObject(cx, &js_ObjectClass, NULL, NULL);
        if (!obj)
            return JS_FALSE;
    }
    *rval = OBJECT_TO_JSVAL(obj);
    return JS_TRUE;
}

/*
 * ObjectOps and Class for with-statement stack objects.
 */
static JSBool
with_LookupProperty(JSContext *cx, JSObject *obj, jsid id, JSObject **objp,
                    JSProperty **propp)
{
    JSObject *proto = OBJ_GET_PROTO(cx, obj);
    if (!proto)
        return js_LookupProperty(cx, obj, id, objp, propp);
    return OBJ_LOOKUP_PROPERTY(cx, proto, id, objp, propp);
}

static JSBool
with_GetProperty(JSContext *cx, JSObject *obj, jsid id, jsval *vp)
{
    JSObject *proto = OBJ_GET_PROTO(cx, obj);
    if (!proto)
        return js_GetProperty(cx, obj, id, vp);
    return OBJ_GET_PROPERTY(cx, proto, id, vp);
}

static JSBool
with_SetProperty(JSContext *cx, JSObject *obj, jsid id, jsval *vp)
{
    JSObject *proto = OBJ_GET_PROTO(cx, obj);
    if (!proto)
        return js_SetProperty(cx, obj, id, vp);
    return OBJ_SET_PROPERTY(cx, proto, id, vp);
}

static JSBool
with_GetAttributes(JSContext *cx, JSObject *obj, jsid id, JSProperty *prop,
                   uintN *attrsp)
{
    JSObject *proto = OBJ_GET_PROTO(cx, obj);
    if (!proto)
        return js_GetAttributes(cx, obj, id, prop, attrsp);
    return OBJ_GET_ATTRIBUTES(cx, proto, id, prop, attrsp);
}

static JSBool
with_SetAttributes(JSContext *cx, JSObject *obj, jsid id, JSProperty *prop,
                   uintN *attrsp)
{
    JSObject *proto = OBJ_GET_PROTO(cx, obj);
    if (!proto)
        return js_SetAttributes(cx, obj, id, prop, attrsp);
    return OBJ_SET_ATTRIBUTES(cx, proto, id, prop, attrsp);
}

static JSBool
with_DeleteProperty(JSContext *cx, JSObject *obj, jsid id, jsval *rval)
{
    JSObject *proto = OBJ_GET_PROTO(cx, obj);
    if (!proto)
        return js_DeleteProperty(cx, obj, id, rval);
    return OBJ_DELETE_PROPERTY(cx, proto, id, rval);
}

static JSBool
with_DefaultValue(JSContext *cx, JSObject *obj, JSType hint, jsval *vp)
{
    JSObject *proto = OBJ_GET_PROTO(cx, obj);
    if (!proto)
        return js_DefaultValue(cx, obj, hint, vp);
    return OBJ_DEFAULT_VALUE(cx, proto, hint, vp);
}

static JSBool
with_Enumerate(JSContext *cx, JSObject *obj, JSIterateOp enum_op,
               jsval *statep, jsid *idp)
{
    JSObject *proto = OBJ_GET_PROTO(cx, obj);
    if (!proto)
        return js_Enumerate(cx, obj, enum_op, statep, idp);
    return OBJ_ENUMERATE(cx, proto, enum_op, statep, idp);
}

static JSBool
with_CheckAccess(JSContext *cx, JSObject *obj, jsid id, JSAccessMode mode,
                 jsval *vp, uintN *attrsp)
{
    JSObject *proto = OBJ_GET_PROTO(cx, obj);
    if (!proto)
        return js_CheckAccess(cx, obj, id, mode, vp, attrsp);
    return OBJ_CHECK_ACCESS(cx, proto, id, mode, vp, attrsp);
}

static JSObject *
with_ThisObject(JSContext *cx, JSObject *obj)
{
    JSObject *proto = OBJ_GET_PROTO(cx, obj);
    if (!proto)
        return obj;
    return OBJ_THIS_OBJECT(cx, proto);
}

JS_FRIEND_DATA(JSObjectOps) js_WithObjectOps = {
    js_NewObjectMap,        js_DestroyObjectMap,
    with_LookupProperty,    js_DefineProperty,
    with_GetProperty,       with_SetProperty,
    with_GetAttributes,     with_SetAttributes,
    with_DeleteProperty,    with_DefaultValue,
    with_Enumerate,         with_CheckAccess,
    with_ThisObject,        NATIVE_DROP_PROPERTY,
    NULL,                   NULL,
    NULL,                   NULL,
    js_SetProtoOrParent,    js_SetProtoOrParent,
    js_Mark,                js_Clear,
    NULL,                   NULL
};

static JSObjectOps *
with_getObjectOps(JSContext *cx, JSClass *clasp)
{
    return &js_WithObjectOps;
}

JSClass js_WithClass = {
    "With",
    JSCLASS_HAS_PRIVATE | JSCLASS_HAS_RESERVED_SLOTS(1) | JSCLASS_IS_ANONYMOUS,
    JS_PropertyStub,  JS_PropertyStub,  JS_PropertyStub,  JS_PropertyStub,
    JS_EnumerateStub, JS_ResolveStub,   JS_ConvertStub,   JS_FinalizeStub,
    with_getObjectOps,
    0,0,0,0,0,0,0
};

JSObject *
js_NewWithObject(JSContext *cx, JSObject *proto, JSObject *parent, jsint depth)
{
    JSObject *obj;

    obj = js_NewObject(cx, &js_WithClass, proto, parent);
    if (!obj)
        return NULL;
    obj->slots[JSSLOT_PRIVATE] = PRIVATE_TO_JSVAL(cx->fp);
    OBJ_SET_BLOCK_DEPTH(cx, obj, depth);
    return obj;
}

JSObject *
js_NewBlockObject(JSContext *cx)
{
    JSObject *obj;

    /*
     * Null obj's proto slot so that Object.prototype.* does not pollute block
     * scopes.  Make sure obj has its own scope too, since clearing proto does
     * not affect OBJ_SCOPE(obj).
     */
    obj = js_NewObject(cx, &js_BlockClass, NULL, NULL);
    if (!obj || !js_GetMutableScope(cx, obj))
        return NULL;
    OBJ_SET_PROTO(cx, obj, NULL);
    return obj;
}

JSObject *
js_CloneBlockObject(JSContext *cx, JSObject *proto, JSObject *parent,
                    JSStackFrame *fp)
{
    JSObject *clone;

    clone = js_NewObject(cx, &js_BlockClass, proto, parent);
    if (!clone)
        return NULL;
    clone->slots[JSSLOT_PRIVATE] = PRIVATE_TO_JSVAL(fp);
    clone->slots[JSSLOT_BLOCK_DEPTH] =
        OBJ_GET_SLOT(cx, proto, JSSLOT_BLOCK_DEPTH);
    return clone;
}

/*
 * XXXblock this reverses a path in the property tree -- try to share
 *          the prototype's scope harder!
 */
JSBool
js_PutBlockObject(JSContext *cx, JSObject *obj)
{
    JSStackFrame *fp;
    uintN depth, slot;
    JSScopeProperty *sprop;

    fp = (JSStackFrame *) JS_GetPrivate(cx, obj);
    JS_ASSERT(fp);
    depth = OBJ_BLOCK_DEPTH(cx, obj);
    for (sprop = OBJ_SCOPE(obj)->lastProp; sprop; sprop = sprop->parent) {
        if (sprop->getter != js_BlockClass.getProperty)
            continue;
        if (!(sprop->flags & SPROP_HAS_SHORTID))
            continue;
        slot = depth + (uintN)sprop->shortid;
        JS_ASSERT(slot < fp->script->depth);
        if (!js_DefineNativeProperty(cx, obj, sprop->id,
                                     fp->spbase[slot], NULL, NULL,
                                     JSPROP_ENUMERATE | JSPROP_PERMANENT,
                                     SPROP_HAS_SHORTID, sprop->shortid,
                                     NULL)) {
            JS_SetPrivate(cx, obj, NULL);
            return JS_FALSE;
        }
    }

    return JS_SetPrivate(cx, obj, NULL);
}

static JSBool
block_getProperty(JSContext *cx, JSObject *obj, jsval id, jsval *vp)
{
    JSStackFrame *fp;
    jsint slot;

    JS_ASSERT(JS_InstanceOf(cx, obj, &js_BlockClass, NULL));
    if (!JSVAL_IS_INT(id))
        return JS_TRUE;

    fp = (JSStackFrame *) JS_GetPrivate(cx, obj);
    if (!fp)
        return JS_TRUE;

    slot = OBJ_BLOCK_DEPTH(cx, obj) + (uint16) JSVAL_TO_INT(id);
    JS_ASSERT((uintN)slot < fp->script->depth);
    *vp = fp->spbase[slot];
    return JS_TRUE;
}

static JSBool
block_setProperty(JSContext *cx, JSObject *obj, jsval id, jsval *vp)
{
    JSStackFrame *fp;
    jsint slot;

    JS_ASSERT(JS_InstanceOf(cx, obj, &js_BlockClass, NULL));
    if (!JSVAL_IS_INT(id))
        return JS_TRUE;

    fp = (JSStackFrame *) JS_GetPrivate(cx, obj);
    if (!fp)
        return JS_TRUE;

    slot = OBJ_BLOCK_DEPTH(cx, obj) + (uint16) JSVAL_TO_INT(id);
    JS_ASSERT((uintN)slot < fp->script->depth);
    fp->spbase[slot] = *vp;
    return JS_TRUE;
}

#if JS_HAS_XDR

#define NO_PARENT_INDEX (jsatomid)-1

jsatomid
FindObjectAtomIndex(JSAtomMap *map, JSObject *obj)
{
    size_t i;
    JSAtom *atom;

    for (i = 0; i < map->length; i++) {
        atom = map->vector[i];
        if (ATOM_KEY(atom) == OBJECT_TO_JSVAL(obj))
            return i;
    }

    return NO_PARENT_INDEX;
}

static JSBool
block_xdrObject(JSXDRState *xdr, JSObject **objp)
{
    JSContext *cx;
    jsatomid parentId;
    JSAtomMap *atomMap;
    JSObject *obj, *parent;
    uint16 depth, count, i;
    uint32 tmp;
    JSTempValueRooter tvr;
    JSScopeProperty *sprop;
    jsid propid;
    JSAtom *atom;
    int16 shortid;
    JSBool ok;

    cx = xdr->cx;
#ifdef __GNUC__
    obj = NULL;         /* quell GCC overwarning */
#endif

    atomMap = &xdr->script->atomMap;
    if (xdr->mode == JSXDR_ENCODE) {
        obj = *objp;
        parent = OBJ_GET_PARENT(cx, obj);
        parentId = FindObjectAtomIndex(atomMap, parent);
        depth = OBJ_BLOCK_DEPTH(cx, obj);
        count = OBJ_BLOCK_COUNT(cx, obj);
        tmp = (uint32)(depth << 16) | count;
    }
#ifdef __GNUC__ /* suppress bogus gcc warnings */
    else count = 0;
#endif

    /* First, XDR the parent atomid. */
    if (!JS_XDRUint32(xdr, &parentId))
        return JS_FALSE;

    if (xdr->mode == JSXDR_DECODE) {
        obj = js_NewBlockObject(cx);
        if (!obj)
            return JS_FALSE;
        *objp = obj;

        /*
         * If there's a parent id, then get the parent out of our script's
         * atomMap. We know that we XDR block object in outer-to-inner order,
         * which means that getting the parent now will work.
         */
        if (parentId == NO_PARENT_INDEX) {
            parent = NULL;
        } else {
            atom = js_GetAtom(cx, atomMap, parentId);
            JS_ASSERT(ATOM_IS_OBJECT(atom));
            parent = ATOM_TO_OBJECT(atom);
        }
        obj->slots[JSSLOT_PARENT] = OBJECT_TO_JSVAL(parent);
    }

    JS_PUSH_SINGLE_TEMP_ROOT(cx, OBJECT_TO_JSVAL(obj), &tvr);

    if (!JS_XDRUint32(xdr, &tmp)) {
        JS_POP_TEMP_ROOT(cx, &tvr);
        return JS_FALSE;
    }

    if (xdr->mode == JSXDR_DECODE) {
        depth = (uint16)(tmp >> 16);
        count = (uint16)tmp;
        obj->slots[JSSLOT_BLOCK_DEPTH] = INT_TO_JSVAL(depth);
    }

    /*
     * XDR the block object's properties. We know that there are 'count'
     * properties to XDR, stored as id/shortid pairs. We do not XDR any
     * non-native properties, only those that the compiler created.
     */
    sprop = NULL;
    ok = JS_TRUE;
    for (i = 0; i < count; i++) {
        if (xdr->mode == JSXDR_ENCODE) {
            /* Find a property to XDR. */
            do {
                /* If sprop is NULL, this is the first property. */
                sprop = sprop ? sprop->parent : OBJ_SCOPE(obj)->lastProp;
            } while (!(sprop->flags & SPROP_HAS_SHORTID));

            JS_ASSERT(sprop->getter == js_BlockClass.getProperty);
            propid = sprop->id;
            JS_ASSERT(JSID_IS_ATOM(propid));
            atom = JSID_TO_ATOM(propid);
            shortid = sprop->shortid;
            JS_ASSERT(shortid >= 0);
        }

        /* XDR the real id, then the shortid. */
        if (!js_XDRStringAtom(xdr, &atom) ||
            !JS_XDRUint16(xdr, (uint16 *)&shortid)) {
            ok = JS_FALSE;
            break;
        }

        if (xdr->mode == JSXDR_DECODE) {
            if (!js_DefineNativeProperty(cx, obj, ATOM_TO_JSID(atom),
                                         JSVAL_VOID, NULL, NULL,
                                         JSPROP_ENUMERATE | JSPROP_PERMANENT,
                                         SPROP_HAS_SHORTID, shortid, NULL)) {
                ok = JS_FALSE;
                break;
            }
        }
    }

    JS_POP_TEMP_ROOT(cx, &tvr);
    return ok;
}

#else
# define block_xdrObject NULL
#endif

JSClass js_BlockClass = {
    "Block",
    JSCLASS_HAS_PRIVATE | JSCLASS_HAS_RESERVED_SLOTS(1) |
    JSCLASS_IS_ANONYMOUS | JSCLASS_HAS_CACHED_PROTO(JSProto_Block),
    JS_PropertyStub,  JS_PropertyStub,  block_getProperty, block_setProperty,
    JS_EnumerateStub, JS_ResolveStub,   JS_ConvertStub,    JS_FinalizeStub,
    NULL, NULL, NULL, NULL, block_xdrObject, NULL, NULL, NULL
};

JSObject*
js_InitBlockClass(JSContext *cx, JSObject* obj)
{
    JSObject *proto;

    proto = JS_InitClass(cx, obj, NULL, &js_BlockClass, NULL, 0, NULL,
                         NULL, NULL, NULL);
    if (!proto)
        return NULL;

    OBJ_SET_PROTO(cx, proto, NULL);
    return proto;
}

JSObject *
js_InitObjectClass(JSContext *cx, JSObject *obj)
{
    JSObject *proto;
    jsval eval;

    proto = JS_InitClass(cx, obj, NULL, &js_ObjectClass, Object, 1,
                         object_props, object_methods, NULL, NULL);
    if (!proto)
        return NULL;

    /* ECMA (15.1.2.1) says 'eval' is also a property of the global object. */
    if (!OBJ_GET_PROPERTY(cx, proto,
                          ATOM_TO_JSID(cx->runtime->atomState.evalAtom),
                          &eval)) {
        return NULL;
    }
    if (!OBJ_DEFINE_PROPERTY(cx, obj,
                             ATOM_TO_JSID(cx->runtime->atomState.evalAtom),
                             eval, NULL, NULL, 0, NULL)) {
        return NULL;
    }

    return proto;
}

void
js_InitObjectMap(JSObjectMap *map, jsrefcount nrefs, JSObjectOps *ops,
                 JSClass *clasp)
{
    map->nrefs = nrefs;
    map->ops = ops;
    map->nslots = JS_INITIAL_NSLOTS;
    map->freeslot = JSSLOT_FREE(clasp);
}

JSObjectMap *
js_NewObjectMap(JSContext *cx, jsrefcount nrefs, JSObjectOps *ops,
                JSClass *clasp, JSObject *obj)
{
    return (JSObjectMap *) js_NewScope(cx, nrefs, ops, clasp, obj);
}

void
js_DestroyObjectMap(JSContext *cx, JSObjectMap *map)
{
    js_DestroyScope(cx, (JSScope *)map);
}

JSObjectMap *
js_HoldObjectMap(JSContext *cx, JSObjectMap *map)
{
    JS_ASSERT(map->nrefs >= 0);
    JS_ATOMIC_INCREMENT(&map->nrefs);
    return map;
}

JSObjectMap *
js_DropObjectMap(JSContext *cx, JSObjectMap *map, JSObject *obj)
{
    JS_ASSERT(map->nrefs > 0);
    JS_ATOMIC_DECREMENT(&map->nrefs);
    if (map->nrefs == 0) {
        map->ops->destroyObjectMap(cx, map);
        return NULL;
    }
    if (MAP_IS_NATIVE(map) && ((JSScope *)map)->object == obj)
        ((JSScope *)map)->object = NULL;
    return map;
}

static jsval *
AllocSlots(JSContext *cx, jsval *slots, uint32 nslots)
{
    size_t nbytes, obytes, minbytes;
    uint32 i, oslots;
    jsval *newslots;

    nbytes = (nslots + 1) * sizeof(jsval);
    if (slots) {
        oslots = slots[-1];
        obytes = (oslots + 1) * sizeof(jsval);
    } else {
        oslots = 0;
        obytes = 0;
    }

    if (nbytes <= GC_NBYTES_MAX) {
        newslots = (jsval *) js_NewGCThing(cx, GCX_PRIVATE, nbytes);
    } else {
        newslots = (jsval *)
                   JS_realloc(cx,
                              (obytes <= GC_NBYTES_MAX) ? NULL : slots - 1,
                              nbytes);
    }
    if (!newslots)
        return NULL;

    if (obytes != 0) {
        /* If either nbytes or obytes fit in a GC-thing, we must copy. */
        minbytes = JS_MIN(nbytes, obytes);
        if (minbytes <= GC_NBYTES_MAX)
            memcpy(newslots + 1, slots, minbytes - sizeof(jsval));

        /* If nbytes are in a GC-thing but obytes aren't, free obytes. */
        if (nbytes <= GC_NBYTES_MAX && obytes > GC_NBYTES_MAX)
            JS_free(cx, slots - 1);

        /* If we're extending an allocation, initialize free slots. */
        if (nslots > oslots) {
            for (i = 1 + oslots; i <= nslots; i++)
                newslots[i] = JSVAL_VOID;
        }
    }

    newslots[0] = nslots;
    return ++newslots;
}

static void
FreeSlots(JSContext *cx, jsval *slots)
{
    size_t nbytes;

    /*
     * NB: We count on smaller GC-things being finalized before larger things
     * that become garbage during the same GC.  Without this assumption, we
     * couldn't load slots[-1] here without possibly loading a gcFreeList link
     * (see struct JSGCThing in jsgc.h).
     */
    nbytes = (slots[-1] + 1) * sizeof(jsval);
    if (nbytes > GC_NBYTES_MAX)
        JS_free(cx, slots - 1);
}

extern JSBool
js_GetClassId(JSContext *cx, JSClass *clasp, jsid *idp)
{
    JSProtoKey key;
    JSAtom *atom;

    key = JSCLASS_CACHED_PROTO_KEY(clasp);
    if (key != JSProto_Null) {
        *idp = INT_TO_JSID(key);
    } else if (clasp->flags & JSCLASS_IS_ANONYMOUS) {
        *idp = INT_TO_JSID(JSProto_Object);
    } else {
        atom = js_Atomize(cx, clasp->name, strlen(clasp->name), 0);
        if (!atom)
            return JS_FALSE;
        *idp = ATOM_TO_JSID(atom);
    }
    return JS_TRUE;
}

JSObject *
js_NewObject(JSContext *cx, JSClass *clasp, JSObject *proto, JSObject *parent)
{
    jsid id;
    JSObject *obj;
    JSObjectOps *ops;
    JSObjectMap *map;
    JSClass *protoclasp;
    uint32 nslots, i;
    jsval *newslots;
    JSTempValueRooter tvr;

    /* Bootstrap the ur-object, and make it the default prototype object. */
    if (!proto) {
        if (!js_GetClassId(cx, clasp, &id))
            return NULL;
        if (!js_GetClassPrototype(cx, parent, id, &proto))
            return NULL;
        if (!proto &&
            !js_GetClassPrototype(cx, parent, INT_TO_JSID(JSProto_Object),
                                  &proto)) {
            return NULL;
        }
    }

    /* Always call the class's getObjectOps hook if it has one. */
    ops = clasp->getObjectOps
          ? clasp->getObjectOps(cx, clasp)
          : &js_ObjectOps;

    /*
     * Allocate a zeroed object from the GC heap.  Do this *after* any other
     * GC-thing allocations under js_GetClassPrototype or clasp->getObjectOps,
     * to avoid displacing the newborn root for obj.
     */
    obj = (JSObject *) js_NewGCThing(cx, GCX_OBJECT, sizeof(JSObject));
    if (!obj)
        return NULL;

    /*
     * Root obj to prevent it from being collected out from under this call.
     * to js_NewObject.  AllocSlots can trigger a finalizer from a last-ditch
     * GC calling JS_ClearNewbornRoots. There's also the possibilty of things
     * happening under the objectHook call-out further below.
     */
    JS_PUSH_TEMP_ROOT_OBJECT(cx, obj, &tvr);

    /*
     * Share proto's map only if it has the same JSObjectOps, and only if
     * proto's class has the same private and reserved slots as obj's map
     * and class have.  We assume that if prototype and object are of the
     * same class, they always have the same number of computed reserved
     * slots (returned via clasp->reserveSlots); otherwise, prototype and
     * object classes must have the same (null or not) reserveSlots hook.
     */
    if (proto &&
        (map = proto->map)->ops == ops &&
        ((protoclasp = OBJ_GET_CLASS(cx, proto)) == clasp ||
         (!((protoclasp->flags ^ clasp->flags) &
            (JSCLASS_HAS_PRIVATE |
             (JSCLASS_RESERVED_SLOTS_MASK << JSCLASS_RESERVED_SLOTS_SHIFT))) &&
          protoclasp->reserveSlots == clasp->reserveSlots)))
    {
        /*
         * Default parent to the parent of the prototype, which was set from
         * the parent of the prototype's constructor.
         */
        if (!parent)
            parent = OBJ_GET_PARENT(cx, proto);

        /* Share the given prototype's map. */
        obj->map = js_HoldObjectMap(cx, map);

        /* Ensure that obj starts with the minimum slots for clasp. */
        nslots = JS_INITIAL_NSLOTS;
    } else {
        /* Leave parent alone.  Allocate a new map for obj. */
        map = ops->newObjectMap(cx, 1, ops, clasp, obj);
        if (!map)
            goto bad;
        obj->map = map;

        /* Let ops->newObjectMap set nslots so as to reserve slots. */
        nslots = map->nslots;
    }

    /* Allocate a slots vector, with a -1'st element telling its length. */
    newslots = AllocSlots(cx, NULL, nslots);
    if (!newslots) {
        js_DropObjectMap(cx, obj->map, obj);
        obj->map = NULL;
        goto bad;
    }

    /* Set the proto, parent, and class properties. */
    newslots[JSSLOT_PROTO] = OBJECT_TO_JSVAL(proto);
    newslots[JSSLOT_PARENT] = OBJECT_TO_JSVAL(parent);
    newslots[JSSLOT_CLASS] = PRIVATE_TO_JSVAL(clasp);

    /* Clear above JSSLOT_CLASS so the GC doesn't load uninitialized memory. */
    for (i = JSSLOT_CLASS + 1; i < nslots; i++)
        newslots[i] = JSVAL_VOID;

    /* Store newslots after initializing all of 'em, just in case. */
    obj->slots = newslots;

    if (cx->runtime->objectHook) {
        JS_KEEP_ATOMS(cx->runtime);
        cx->runtime->objectHook(cx, obj, JS_TRUE, cx->runtime->objectHookData);
        JS_UNKEEP_ATOMS(cx->runtime);
    }

out:
    JS_POP_TEMP_ROOT(cx, &tvr);
    cx->weakRoots.newborn[GCX_OBJECT] = (JSGCThing *) obj;
    return obj;

bad:
    obj = NULL;
    goto out;
}

JS_STATIC_DLL_CALLBACK(JSObject *)
js_InitNullClass(JSContext *cx, JSObject *obj)
{
    JS_ASSERT(0);
    return NULL;
}

#define JS_PROTO(name,code,init) extern JSObject *init(JSContext *, JSObject *);
#include "jsproto.tbl"
#undef JS_PROTO

static JSObjectOp lazy_prototype_init[JSProto_LIMIT] = {
#define JS_PROTO(name,code,init) init,
#include "jsproto.tbl"
#undef JS_PROTO
};

JSBool
js_GetClassObject(JSContext *cx, JSObject *obj, JSProtoKey key,
                  JSObject **objp)
{
    JSBool ok;
    JSObject *tmp, *cobj;
    JSResolvingKey rkey;
    JSResolvingEntry *rentry;
    uint32 generation;
    JSObjectOp init;
    jsval v;

    while ((tmp = OBJ_GET_PARENT(cx, obj)) != NULL)
        obj = tmp;
    if (!(OBJ_GET_CLASS(cx, obj)->flags & JSCLASS_IS_GLOBAL)) {
        *objp = NULL;
        return JS_TRUE;
    }

    ok = JS_GetReservedSlot(cx, obj, key, &v);
    if (!ok)
        return JS_FALSE;
    if (!JSVAL_IS_PRIMITIVE(v)) {
        *objp = JSVAL_TO_OBJECT(v);
        return JS_TRUE;
    }

    rkey.obj = obj;
    rkey.id = ATOM_TO_JSID(cx->runtime->atomState.classAtoms[key]);
    if (!js_StartResolving(cx, &rkey, JSRESFLAG_LOOKUP, &rentry))
        return JS_FALSE;
    if (!rentry) {
        /* Already caching key in obj -- suppress recursion. */
        *objp = NULL;
        return JS_TRUE;
    }
    generation = cx->resolvingTable->generation;

    cobj = NULL;
    init = lazy_prototype_init[key];
    if (init) {
        if (!init(cx, obj)) {
            ok = JS_FALSE;
        } else {
            ok = JS_GetReservedSlot(cx, obj, key, &v);
            if (ok && !JSVAL_IS_PRIMITIVE(v))
                cobj = JSVAL_TO_OBJECT(v);
        }
    }

    js_StopResolving(cx, &rkey, JSRESFLAG_LOOKUP, rentry, generation);
    *objp = cobj;
    return ok;
}

JSBool
js_SetClassObject(JSContext *cx, JSObject *obj, JSProtoKey key, JSObject *cobj)
{
    JS_ASSERT(!OBJ_GET_PARENT(cx, obj));
    if (!(OBJ_GET_CLASS(cx, obj)->flags & JSCLASS_IS_GLOBAL))
        return JS_TRUE;

    return JS_SetReservedSlot(cx, obj, key, OBJECT_TO_JSVAL(cobj));
}

JSBool
js_FindClassObject(JSContext *cx, JSObject *start, jsid id, jsval *vp)
{
    JSObject *obj, *cobj, *pobj;
    JSProtoKey key;
    JSProperty *prop;
    JSScopeProperty *sprop;

    if (start || (cx->fp && (start = cx->fp->scopeChain) != NULL)) {
        /* Find the topmost object in the scope chain. */
        do {
            obj = start;
            start = OBJ_GET_PARENT(cx, obj);
        } while (start);
    } else {
        obj = cx->globalObject;
        if (!obj) {
            *vp = JSVAL_VOID;
            return JS_TRUE;
        }
    }

    OBJ_TO_INNER_OBJECT(cx, obj);
    if (!obj)
        return JS_FALSE;

    if (JSID_IS_INT(id)) {
        key = JSID_TO_INT(id);
        JS_ASSERT(key != JSProto_Null);
        if (!js_GetClassObject(cx, obj, key, &cobj))
            return JS_FALSE;
        if (cobj) {
            *vp = OBJECT_TO_JSVAL(cobj);
            return JS_TRUE;
        }
        id = ATOM_TO_JSID(cx->runtime->atomState.classAtoms[key]);
    }

    JS_ASSERT(OBJ_IS_NATIVE(obj));
    if (!js_LookupPropertyWithFlags(cx, obj, id, JSRESOLVE_CLASSNAME,
                                    &pobj, &prop)) {
        return JS_FALSE;
    }
    if (!prop)  {
        *vp = JSVAL_VOID;
        return JS_TRUE;
    }

    JS_ASSERT(OBJ_IS_NATIVE(pobj));
    sprop = (JSScopeProperty *) prop;
    JS_ASSERT(SPROP_HAS_VALID_SLOT(sprop, OBJ_SCOPE(pobj)));
    *vp = OBJ_GET_SLOT(cx, pobj, sprop->slot);
    OBJ_DROP_PROPERTY(cx, pobj, prop);
    return JS_TRUE;
}

JSObject *
js_ConstructObject(JSContext *cx, JSClass *clasp, JSObject *proto,
                   JSObject *parent, uintN argc, jsval *argv)
{
    jsid id;
    jsval cval, rval;
    JSTempValueRooter argtvr, tvr;
    JSObject *obj, *ctor;

    JS_PUSH_TEMP_ROOT(cx, argc, argv, &argtvr);

    if (!js_GetClassId(cx, clasp, &id) ||
        !js_FindClassObject(cx, parent, id, &cval)) {
        JS_POP_TEMP_ROOT(cx, &argtvr);
        return NULL;
    }

    if (JSVAL_IS_PRIMITIVE(cval)) {
        js_ReportIsNotFunction(cx, &cval, JSV2F_CONSTRUCT | JSV2F_SEARCH_STACK);
        JS_POP_TEMP_ROOT(cx, &argtvr);
        return NULL;
    }

    /*
     * Protect cval in case a crazy getter for .prototype uproots it.  After
     * this point, all control flow must exit through label out with obj set.
     */
    JS_PUSH_SINGLE_TEMP_ROOT(cx, cval, &tvr);

    /*
     * If proto or parent are NULL, set them to Constructor.prototype and/or
     * Constructor.__parent__, just like JSOP_NEW does.
     */
    ctor = JSVAL_TO_OBJECT(cval);
    if (!parent)
        parent = OBJ_GET_PARENT(cx, ctor);
    if (!proto) {
        if (!OBJ_GET_PROPERTY(cx, ctor,
                              ATOM_TO_JSID(cx->runtime->atomState
                                           .classPrototypeAtom),
                              &rval)) {
            obj = NULL;
            goto out;
        }
        if (JSVAL_IS_OBJECT(rval))
            proto = JSVAL_TO_OBJECT(rval);
    }

    obj = js_NewObject(cx, clasp, proto, parent);
    if (!obj)
        goto out;

    if (!js_InternalConstruct(cx, obj, cval, argc, argv, &rval))
        goto bad;

    if (JSVAL_IS_PRIMITIVE(rval))
        goto out;
    obj = JSVAL_TO_OBJECT(rval);

    /*
     * If the instance's class differs from what was requested, throw a type
     * error.  If the given class has both the JSCLASS_HAS_PRIVATE and the
     * JSCLASS_CONSTRUCT_PROTOTYPE flags, and the instance does not have its
     * private data set at this point, then the constructor was replaced and
     * we should throw a type error.
     */
    if (OBJ_GET_CLASS(cx, obj) != clasp ||
        (!(~clasp->flags & (JSCLASS_HAS_PRIVATE |
                            JSCLASS_CONSTRUCT_PROTOTYPE)) &&
         !JS_GetPrivate(cx, obj))) {
        JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL,
                             JSMSG_WRONG_CONSTRUCTOR, clasp->name);
        goto bad;
    }

out:
    JS_POP_TEMP_ROOT(cx, &tvr);
    JS_POP_TEMP_ROOT(cx, &argtvr);
    return obj;

bad:
    cx->weakRoots.newborn[GCX_OBJECT] = NULL;
    obj = NULL;
    goto out;
}

void
js_FinalizeObject(JSContext *cx, JSObject *obj)
{
    JSObjectMap *map;

    /* Cope with stillborn objects that have no map. */
    map = obj->map;
    if (!map)
        return;
    JS_ASSERT(obj->slots);

    if (cx->runtime->objectHook)
        cx->runtime->objectHook(cx, obj, JS_FALSE, cx->runtime->objectHookData);

    /* Remove all watchpoints with weak links to obj. */
    JS_ClearWatchPointsForObject(cx, obj);

    /*
     * Finalize obj first, in case it needs map and slots.  Optimized to use
     * LOCKED_OBJ_GET_CLASS instead of OBJ_GET_CLASS, so we avoid "promoting"
     * obj's scope from lock-free to lock-full (see jslock.c:ClaimScope) when
     * we're called from the GC.  Only the GC should call js_FinalizeObject,
     * and no other threads run JS (and possibly racing to update obj->slots)
     * while the GC is running.
     */
    LOCKED_OBJ_GET_CLASS(obj)->finalize(cx, obj);

    /* Drop map and free slots. */
    js_DropObjectMap(cx, map, obj);
    obj->map = NULL;
    FreeSlots(cx, obj->slots);
    obj->slots = NULL;
}

/* XXXbe if one adds props, deletes earlier props, adds more, the last added
         won't recycle the deleted props' slots. */
JSBool
js_AllocSlot(JSContext *cx, JSObject *obj, uint32 *slotp)
{
    JSObjectMap *map;
    JSClass *clasp;
    uint32 nslots;
    jsval *newslots;

    map = obj->map;
    JS_ASSERT(!MAP_IS_NATIVE(map) || ((JSScope *)map)->object == obj);
    clasp = LOCKED_OBJ_GET_CLASS(obj);
    if (map->freeslot == JSSLOT_FREE(clasp)) {
        /* Adjust map->freeslot to include computed reserved slots, if any. */
        if (clasp->reserveSlots)
            map->freeslot += clasp->reserveSlots(cx, obj);
    }
    nslots = map->nslots;
    if (map->freeslot >= nslots) {
        nslots = map->freeslot;
        JS_ASSERT(nslots >= JS_INITIAL_NSLOTS);
        nslots += (nslots + 1) / 2;

        newslots = AllocSlots(cx, obj->slots, nslots);
        if (!newslots)
            return JS_FALSE;
        map->nslots = nslots;
        obj->slots = newslots;
    }

#ifdef TOO_MUCH_GC
    obj->slots[map->freeslot] = JSVAL_VOID;
#endif
    *slotp = map->freeslot++;
    return JS_TRUE;
}

void
js_FreeSlot(JSContext *cx, JSObject *obj, uint32 slot)
{
    JSObjectMap *map;
    uint32 nslots;
    jsval *newslots;

    OBJ_CHECK_SLOT(obj, slot);
    obj->slots[slot] = JSVAL_VOID;
    map = obj->map;
    JS_ASSERT(!MAP_IS_NATIVE(map) || ((JSScope *)map)->object == obj);
    if (map->freeslot == slot + 1)
        map->freeslot = slot;
    nslots = map->nslots;
    if (nslots > JS_INITIAL_NSLOTS && map->freeslot < nslots / 2) {
        nslots = map->freeslot;
        nslots += nslots / 2;
        if (nslots < JS_INITIAL_NSLOTS)
            nslots = JS_INITIAL_NSLOTS;

        newslots = AllocSlots(cx, obj->slots, nslots);
        if (!newslots)
            return;
        map->nslots = nslots;
        obj->slots = newslots;
    }
}

/* JSVAL_INT_MAX as a string */
#define JSVAL_INT_MAX_STRING "1073741823"

#define CHECK_FOR_STRING_INDEX(id)                                            \
    JS_BEGIN_MACRO                                                            \
        if (JSID_IS_ATOM(id)) {                                               \
            JSAtom *atom_ = JSID_TO_ATOM(id);                                 \
            JSString *str_ = ATOM_TO_STRING(atom_);                           \
            const jschar *cp_ = str_->chars;                                  \
            JSBool negative_ = (*cp_ == '-');                                 \
            if (negative_) cp_++;                                             \
            if (JS7_ISDEC(*cp_)) {                                            \
                size_t n_ = str_->length - negative_;                         \
                if (n_ <= sizeof(JSVAL_INT_MAX_STRING) - 1)                   \
                    id = CheckForStringIndex(id, cp_, cp_ + n_, negative_);   \
            }                                                                 \
        }                                                                     \
    JS_END_MACRO

static jsid
CheckForStringIndex(jsid id, const jschar *cp, const jschar *end,
                    JSBool negative)
{
    jsuint index = JS7_UNDEC(*cp++);
    jsuint oldIndex = 0;
    jsuint c = 0;

    if (index != 0) {
        while (JS7_ISDEC(*cp)) {
            oldIndex = index;
            c = JS7_UNDEC(*cp);
            index = 10 * index + c;
            cp++;
        }
    }
    if (cp == end &&
        (oldIndex < (JSVAL_INT_MAX / 10) ||
         (oldIndex == (JSVAL_INT_MAX / 10) &&
          c <= (JSVAL_INT_MAX % 10)))) {
        if (negative)
            index = 0 - index;
        id = INT_TO_JSID((jsint)index);
    }
    return id;
}

static JSBool
HidePropertyName(JSContext *cx, jsid *idp)
{
    jsid id;
    JSAtom *atom, *hidden;

    id = *idp;
    JS_ASSERT(JSID_IS_ATOM(id));

    atom = JSID_TO_ATOM(id);
    JS_ASSERT(!(atom->flags & ATOM_HIDDEN));
    JS_ASSERT(ATOM_IS_STRING(atom));

    hidden = js_AtomizeString(cx, ATOM_TO_STRING(atom), ATOM_HIDDEN);
    if (!hidden)
        return JS_FALSE;

    /*
     * Link hidden to unhidden atom to optimize call_enumerate -- this means
     * the GC must mark a hidden atom's unhidden counterpart (see js_MarkAtom
     * in jsgc.c).  It uses the atom's entry.value member for this linkage.
     */
    hidden->entry.value = atom;
    *idp = ATOM_TO_JSID(hidden);
    return JS_TRUE;
}

JSScopeProperty *
js_AddHiddenProperty(JSContext *cx, JSObject *obj, jsid id,
                     JSPropertyOp getter, JSPropertyOp setter, uint32 slot,
                     uintN attrs, uintN flags, intN shortid)
{
    if (!HidePropertyName(cx, &id))
        return NULL;

    flags |= SPROP_IS_HIDDEN;
    return js_AddNativeProperty(cx, obj, id, getter, setter, slot, attrs,
                                flags, shortid);
}

JSBool
js_LookupHiddenProperty(JSContext *cx, JSObject *obj, jsid id, JSObject **objp,
                        JSProperty **propp)
{
    return HidePropertyName(cx, &id) &&
           js_LookupPropertyWithFlags(cx, obj, id, JSRESOLVE_HIDDEN,
                                      objp, propp);
}

JSScopeProperty *
js_AddNativeProperty(JSContext *cx, JSObject *obj, jsid id,
                     JSPropertyOp getter, JSPropertyOp setter, uint32 slot,
                     uintN attrs, uintN flags, intN shortid)
{
    JSScope *scope;
    JSScopeProperty *sprop;

    JS_LOCK_OBJ(cx, obj);
    scope = js_GetMutableScope(cx, obj);
    if (!scope) {
        sprop = NULL;
    } else {
        /*
         * Handle old bug that took empty string as zero index.  Also convert
         * string indices to integers if appropriate.
         */
        CHECK_FOR_STRING_INDEX(id);
        sprop = js_AddScopeProperty(cx, scope, id, getter, setter, slot, attrs,
                                    flags, shortid);
    }
    JS_UNLOCK_OBJ(cx, obj);
    return sprop;
}

JSScopeProperty *
js_ChangeNativePropertyAttrs(JSContext *cx, JSObject *obj,
                             JSScopeProperty *sprop, uintN attrs, uintN mask,
                             JSPropertyOp getter, JSPropertyOp setter)
{
    JSScope *scope;

    JS_LOCK_OBJ(cx, obj);
    scope = js_GetMutableScope(cx, obj);
    if (!scope) {
        sprop = NULL;
    } else {
        sprop = js_ChangeScopePropertyAttrs(cx, scope, sprop, attrs, mask,
                                            getter, setter);
        if (sprop) {
            PROPERTY_CACHE_FILL(&cx->runtime->propertyCache, obj, sprop->id,
                                sprop);
        }
    }
    JS_UNLOCK_OBJ(cx, obj);
    return sprop;
}

JSBool
js_DefineProperty(JSContext *cx, JSObject *obj, jsid id, jsval value,
                  JSPropertyOp getter, JSPropertyOp setter, uintN attrs,
                  JSProperty **propp)
{
    return js_DefineNativeProperty(cx, obj, id, value, getter, setter, attrs,
                                   0, 0, propp);
}

/*
 * Backward compatibility requires allowing addProperty hooks to mutate the
 * nominal initial value of a slot-full property, while GC safety wants that
 * value to be stored before the call-out through the hook.  Optimize to do
 * both while saving cycles for classes that stub their addProperty hook.
 */
#define ADD_PROPERTY_HELPER(cx,clasp,obj,scope,sprop,vp,cleanup)              \
    JS_BEGIN_MACRO                                                            \
        if ((clasp)->addProperty != JS_PropertyStub) {                        \
            jsval nominal_ = *(vp);                                           \
            if (!(clasp)->addProperty(cx, obj, SPROP_USERID(sprop), vp)) {    \
                cleanup;                                                      \
            }                                                                 \
            if (*(vp) != nominal_) {                                          \
                if (SPROP_HAS_VALID_SLOT(sprop, scope))                       \
                    LOCKED_OBJ_SET_SLOT(obj, (sprop)->slot, *(vp));           \
            }                                                                 \
        }                                                                     \
    JS_END_MACRO

JSBool
js_DefineNativeProperty(JSContext *cx, JSObject *obj, jsid id, jsval value,
                        JSPropertyOp getter, JSPropertyOp setter, uintN attrs,
                        uintN flags, intN shortid, JSProperty **propp)
{
    JSClass *clasp;
    JSScope *scope;
    JSScopeProperty *sprop;

    /*
     * Handle old bug that took empty string as zero index.  Also convert
     * string indices to integers if appropriate.
     */
    CHECK_FOR_STRING_INDEX(id);

#if JS_HAS_GETTER_SETTER
    /*
     * If defining a getter or setter, we must check for its counterpart and
     * update the attributes and property ops.  A getter or setter is really
     * only half of a property.
     */
    if (attrs & (JSPROP_GETTER | JSPROP_SETTER)) {
        JSObject *pobj;
        JSProperty *prop;

        /*
         * If JS_THREADSAFE and id is found, js_LookupProperty returns with
         * sprop non-null and pobj locked.  If pobj == obj, the property is
         * already in obj and obj has its own (mutable) scope.  So if we are
         * defining a getter whose setter was already defined, or vice versa,
         * finish the job via js_ChangeScopePropertyAttributes, and refresh
         * the property cache line for (obj, id) to map sprop.
         */
        if (!js_LookupProperty(cx, obj, id, &pobj, &prop))
            return JS_FALSE;
        sprop = (JSScopeProperty *) prop;
        if (sprop &&
            pobj == obj &&
            (sprop->attrs & (JSPROP_GETTER | JSPROP_SETTER))) {
            sprop = js_ChangeScopePropertyAttrs(cx, OBJ_SCOPE(obj), sprop,
                                                attrs, sprop->attrs,
                                                (attrs & JSPROP_GETTER)
                                                ? getter
                                                : sprop->getter,
                                                (attrs & JSPROP_SETTER)
                                                ? setter
                                                : sprop->setter);

            /* NB: obj == pobj, so we can share unlock code at the bottom. */
            if (!sprop)
                goto bad;
            goto out;
        }

        if (prop) {
            /* NB: call OBJ_DROP_PROPERTY, as pobj might not be native. */
            OBJ_DROP_PROPERTY(cx, pobj, prop);
            prop = NULL;
        }
    }
#endif /* JS_HAS_GETTER_SETTER */

    /* Lock if object locking is required by this implementation. */
    JS_LOCK_OBJ(cx, obj);

    /* Use the object's class getter and setter by default. */
    clasp = LOCKED_OBJ_GET_CLASS(obj);
    if (!getter)
        getter = clasp->getProperty;
    if (!setter)
        setter = clasp->setProperty;

    /* Get obj's own scope if it has one, or create a new one for obj. */
    scope = js_GetMutableScope(cx, obj);
    if (!scope)
        goto bad;

    /* Add the property to scope, or replace an existing one of the same id. */
    if (clasp->flags & JSCLASS_SHARE_ALL_PROPERTIES)
        attrs |= JSPROP_SHARED;
    sprop = js_AddScopeProperty(cx, scope, id, getter, setter,
                                SPROP_INVALID_SLOT, attrs, flags, shortid);
    if (!sprop)
        goto bad;

    /* Store value before calling addProperty, in case the latter GC's. */
    if (SPROP_HAS_VALID_SLOT(sprop, scope))
        LOCKED_OBJ_SET_SLOT(obj, sprop->slot, value);

    /* XXXbe called with lock held */
    ADD_PROPERTY_HELPER(cx, clasp, obj, scope, sprop, &value,
                        js_RemoveScopeProperty(cx, scope, id);
                        goto bad);

#if JS_HAS_GETTER_SETTER
out:
#endif
    PROPERTY_CACHE_FILL(&cx->runtime->propertyCache, obj, id, sprop);
    if (propp)
        *propp = (JSProperty *) sprop;
    else
        JS_UNLOCK_OBJ(cx, obj);
    return JS_TRUE;

bad:
    JS_UNLOCK_OBJ(cx, obj);
    return JS_FALSE;
}

/*
 * Given pc pointing after a property accessing bytecode, return true if the
 * access is "object-detecting" in the sense used by web scripts, e.g., when
 * checking whether document.all is defined.
 */
static JSBool
Detecting(JSContext *cx, jsbytecode *pc)
{
    JSScript *script;
    jsbytecode *endpc;
    JSOp op;
    JSAtom *atom;

    if (!cx->fp)
        return JS_FALSE;
    script = cx->fp->script;
    for (endpc = script->code + script->length; pc < endpc; pc++) {
        /* General case: a branch or equality op follows the access. */
        op = (JSOp) *pc;
        if (js_CodeSpec[op].format & JOF_DETECTING)
            return JS_TRUE;

        /*
         * Special case #1: handle (document.all == null).  Don't sweat about
         * JS1.2's revision of the equality operators here.
         */
        if (op == JSOP_NULL) {
            if (++pc < endpc)
                return *pc == JSOP_EQ || *pc == JSOP_NE;
            break;
        }

        /*
         * Special case #2: handle (document.all == undefined).  Don't worry
         * about someone redefining undefined, which was added by Edition 3,
         * so is read/write for backward compatibility.
         */
        if (op == JSOP_NAME) {
            atom = GET_ATOM(cx, script, pc);
            if (atom == cx->runtime->atomState.typeAtoms[JSTYPE_VOID] &&
                (pc += js_CodeSpec[op].length) < endpc) {
                op = (JSOp) *pc;
                return op == JSOP_EQ || op == JSOP_NE ||
                       op == JSOP_NEW_EQ || op == JSOP_NEW_NE;
            }
            break;
        }

        /* At this point, anything but grouping means we're not detecting. */
        if (op != JSOP_GROUP)
            break;
    }
    return JS_FALSE;
}

JS_FRIEND_API(JSBool)
js_LookupProperty(JSContext *cx, JSObject *obj, jsid id, JSObject **objp,
                  JSProperty **propp)
{
    return js_LookupPropertyWithFlags(cx, obj, id, 0, objp, propp);
}

JSBool
js_LookupPropertyWithFlags(JSContext *cx, JSObject *obj, jsid id, uintN flags,
                           JSObject **objp, JSProperty **propp)
{
    JSObject *start, *obj2, *proto;
    JSScope *scope;
    JSScopeProperty *sprop;
    JSClass *clasp;
    JSResolveOp resolve;
    JSResolvingKey key;
    JSResolvingEntry *entry;
    uint32 generation;
    JSNewResolveOp newresolve;
    jsbytecode *pc;
    const JSCodeSpec *cs;
    uint32 format;
    JSBool ok;

    /*
     * Handle old bug that took empty string as zero index.  Also convert
     * string indices to integers if appropriate.
     */
    CHECK_FOR_STRING_INDEX(id);

    /* Search scopes starting with obj and following the prototype link. */
    start = obj;
    for (;;) {
        JS_LOCK_OBJ(cx, obj);
        scope = OBJ_SCOPE(obj);
        if (scope->object == obj) {
            sprop = SCOPE_GET_PROPERTY(scope, id);
        } else {
            /* Shared prototype scope: try resolve before lookup. */
            sprop = NULL;
        }

        /* Try obj's class resolve hook if id was not found in obj's scope. */
        if (!sprop) {
            clasp = LOCKED_OBJ_GET_CLASS(obj);
            resolve = clasp->resolve;
            if (resolve != JS_ResolveStub) {
                /* Avoid recursion on (obj, id) already being resolved on cx. */
                key.obj = obj;
                key.id = id;

                /*
                 * Once we have successfully added an entry for (obj, key) to
                 * cx->resolvingTable, control must go through cleanup: before
                 * returning.  But note that JS_DHASH_ADD may find an existing
                 * entry, in which case we bail to suppress runaway recursion.
                 */
                if (!js_StartResolving(cx, &key, JSRESFLAG_LOOKUP, &entry)) {
                    JS_UNLOCK_OBJ(cx, obj);
                    return JS_FALSE;
                }
                if (!entry) {
                    /* Already resolving id in obj -- suppress recursion. */
                    JS_UNLOCK_OBJ(cx, obj);
                    goto out;
                }
                generation = cx->resolvingTable->generation;

                /* Null *propp here so we can test it at cleanup: safely. */
                *propp = NULL;

                if (clasp->flags & JSCLASS_NEW_RESOLVE) {
                    newresolve = (JSNewResolveOp)resolve;
                    if (!(flags & JSRESOLVE_CLASSNAME) &&
                        cx->fp &&
                        (pc = cx->fp->pc)) {
                        cs = &js_CodeSpec[*pc];
                        format = cs->format;
                        if ((format & JOF_MODEMASK) != JOF_NAME)
                            flags |= JSRESOLVE_QUALIFIED;
                        if ((format & JOF_ASSIGNING) ||
                            (cx->fp->flags & JSFRAME_ASSIGNING)) {
                            flags |= JSRESOLVE_ASSIGNING;
                        } else {
                            pc += cs->length;
                            if (Detecting(cx, pc))
                                flags |= JSRESOLVE_DETECTING;
                        }
                        if (format & JOF_DECLARING)
                            flags |= JSRESOLVE_DECLARING;
                    }
                    obj2 = (clasp->flags & JSCLASS_NEW_RESOLVE_GETS_START)
                           ? start
                           : NULL;
                    JS_UNLOCK_OBJ(cx, obj);

                    /* Protect id and all atoms from a GC nested in resolve. */
                    JS_KEEP_ATOMS(cx->runtime);
                    ok = newresolve(cx, obj, ID_TO_VALUE(id), flags, &obj2);
                    JS_UNKEEP_ATOMS(cx->runtime);
                    if (!ok)
                        goto cleanup;

                    JS_LOCK_OBJ(cx, obj);
                    if (obj2) {
                        /* Resolved: juggle locks and lookup id again. */
                        if (obj2 != obj) {
                            JS_UNLOCK_OBJ(cx, obj);
                            JS_LOCK_OBJ(cx, obj2);
                        }
                        scope = OBJ_SCOPE(obj2);
                        if (!MAP_IS_NATIVE(&scope->map)) {
                            /* Whoops, newresolve handed back a foreign obj2. */
                            JS_ASSERT(obj2 != obj);
                            JS_UNLOCK_OBJ(cx, obj2);
                            ok = OBJ_LOOKUP_PROPERTY(cx, obj2, id, objp, propp);
                            if (!ok || *propp)
                                goto cleanup;
                            JS_LOCK_OBJ(cx, obj2);
                        } else {
                            /*
                             * Require that obj2 have its own scope now, as we
                             * do for old-style resolve.  If it doesn't, then
                             * id was not truly resolved, and we'll find it in
                             * the proto chain, or miss it if obj2's proto is
                             * not on obj's proto chain.  That last case is a
                             * "too bad!" case.
                             */
                            if (scope->object == obj2)
                                sprop = SCOPE_GET_PROPERTY(scope, id);
                        }
                        if (sprop) {
                            JS_ASSERT(obj2 == scope->object);
                            obj = obj2;
                        } else if (obj2 != obj) {
                            JS_UNLOCK_OBJ(cx, obj2);
                            JS_LOCK_OBJ(cx, obj);
                        }
                    }
                } else {
                    /*
                     * Old resolve always requires id re-lookup if obj owns
                     * its scope after resolve returns.
                     */
                    JS_UNLOCK_OBJ(cx, obj);
                    ok = resolve(cx, obj, ID_TO_VALUE(id));
                    if (!ok)
                        goto cleanup;
                    JS_LOCK_OBJ(cx, obj);
                    scope = OBJ_SCOPE(obj);
                    JS_ASSERT(MAP_IS_NATIVE(&scope->map));
                    if (scope->object == obj)
                        sprop = SCOPE_GET_PROPERTY(scope, id);
                }

            cleanup:
                js_StopResolving(cx, &key, JSRESFLAG_LOOKUP, entry, generation);
                if (!ok || *propp)
                    return ok;
            }
        }

        if (sprop) {
            JS_ASSERT(OBJ_SCOPE(obj) == scope);
            *objp = scope->object;      /* XXXbe hide in jsscope.[ch] */

            *propp = (JSProperty *) sprop;
            return JS_TRUE;
        }

        proto = LOCKED_OBJ_GET_PROTO(obj);
        JS_UNLOCK_OBJ(cx, obj);
        if (!proto)
            break;
        if (!OBJ_IS_NATIVE(proto))
            return OBJ_LOOKUP_PROPERTY(cx, proto, id, objp, propp);
        obj = proto;
    }

out:
    *objp = NULL;
    *propp = NULL;
    return JS_TRUE;
}

JS_FRIEND_API(JSBool)
js_FindProperty(JSContext *cx, jsid id, JSObject **objp, JSObject **pobjp,
                JSProperty **propp)
{
    JSRuntime *rt;
    JSObject *obj, *pobj, *lastobj;
    JSScopeProperty *sprop;
    JSProperty *prop;

    rt = cx->runtime;
    obj = cx->fp->scopeChain;
    do {
        /* Try the property cache and return immediately on cache hit. */
        if (OBJ_IS_NATIVE(obj)) {
            JS_LOCK_OBJ(cx, obj);
            PROPERTY_CACHE_TEST(&rt->propertyCache, obj, id, sprop);
            if (sprop) {
                JS_ASSERT(OBJ_IS_NATIVE(obj));
                *objp = obj;
                *pobjp = obj;
                *propp = (JSProperty *) sprop;
                return JS_TRUE;
            }
            JS_UNLOCK_OBJ(cx, obj);
        }

        /* If cache miss, take the slow path. */
        if (!OBJ_LOOKUP_PROPERTY(cx, obj, id, &pobj, &prop))
            return JS_FALSE;
        if (prop) {
            if (OBJ_IS_NATIVE(pobj)) {
                sprop = (JSScopeProperty *) prop;
                PROPERTY_CACHE_FILL(&rt->propertyCache, pobj, id, sprop);
            }
            *objp = obj;
            *pobjp = pobj;
            *propp = prop;
            return JS_TRUE;
        }
        lastobj = obj;
    } while ((obj = OBJ_GET_PARENT(cx, obj)) != NULL);

    *objp = lastobj;
    *pobjp = NULL;
    *propp = NULL;
    return JS_TRUE;
}

JSObject *
js_FindIdentifierBase(JSContext *cx, jsid id)
{
    JSObject *obj, *pobj;
    JSProperty *prop;

    /*
     * Look for id's property along the "with" statement chain and the
     * statically-linked scope chain.
     */
    if (!js_FindProperty(cx, id, &obj, &pobj, &prop))
        return NULL;
    if (prop) {
        OBJ_DROP_PROPERTY(cx, pobj, prop);
        return obj;
    }

    /*
     * Use the top-level scope from the scope chain, which won't end in the
     * same scope as cx->globalObject for cross-context function calls.
     */
    JS_ASSERT(obj);

    /*
     * Property not found.  Give a strict warning if binding an undeclared
     * top-level variable.
     */
    if (JS_HAS_STRICT_OPTION(cx)) {
        JSString *str = JSVAL_TO_STRING(ID_TO_VALUE(id));
        if (!JS_ReportErrorFlagsAndNumber(cx,
                                          JSREPORT_WARNING | JSREPORT_STRICT,
                                          js_GetErrorMessage, NULL,
                                          JSMSG_UNDECLARED_VAR,
                                          JS_GetStringBytes(str))) {
            return NULL;
        }
    }
    return obj;
}

JSBool
js_NativeGet(JSContext *cx, JSObject *obj, JSObject *pobj,
             JSScopeProperty *sprop, jsval *vp)
{
    JSScope *scope;
    uint32 slot;
    int32 sample;
    JSTempValueRooter tvr;
    JSBool ok;

    JS_ASSERT(OBJ_IS_NATIVE(pobj));
    JS_ASSERT(JS_IS_OBJ_LOCKED(cx, pobj));
    scope = OBJ_SCOPE(pobj);
    JS_ASSERT(scope->object == pobj);

    slot = sprop->slot;
    *vp = (slot != SPROP_INVALID_SLOT)
          ? LOCKED_OBJ_GET_SLOT(pobj, slot)
          : JSVAL_VOID;
    if (SPROP_HAS_STUB_GETTER(sprop))
        return JS_TRUE;

    sample = cx->runtime->propertyRemovals;
    JS_UNLOCK_SCOPE(cx, scope);
    JS_PUSH_TEMP_ROOT_SPROP(cx, sprop, &tvr);
    ok = SPROP_GET(cx, sprop, obj, pobj, vp);
    JS_POP_TEMP_ROOT(cx, &tvr);
    if (!ok)
        return JS_FALSE;

    JS_LOCK_SCOPE(cx, scope);
    JS_ASSERT(scope->object == pobj);
    if (SLOT_IN_SCOPE(slot, scope) &&
        (JS_LIKELY(cx->runtime->propertyRemovals == sample) ||
         SCOPE_GET_PROPERTY(scope, sprop->id) == sprop)) {
        LOCKED_OBJ_SET_SLOT(pobj, slot, *vp);
    }

    return JS_TRUE;
}

JSBool
js_NativeSet(JSContext *cx, JSObject *obj, JSScopeProperty *sprop, jsval *vp)
{
    JSScope *scope;
    uint32 slot;
    jsval pval;
    int32 sample;
    JSTempValueRooter tvr;
    JSBool ok;

    JS_ASSERT(OBJ_IS_NATIVE(obj));
    JS_ASSERT(JS_IS_OBJ_LOCKED(cx, obj));
    scope = OBJ_SCOPE(obj);
    JS_ASSERT(scope->object == obj);

    slot = sprop->slot;
    if (slot != SPROP_INVALID_SLOT) {
        pval = LOCKED_OBJ_GET_SLOT(obj, slot);

        /* If sprop has a stub setter, keep scope locked and just store *vp. */
        if (SPROP_HAS_STUB_SETTER(sprop))
            goto set_slot;
    } else {
        /*
         * Allow API consumers to create shared properties with stub setters.
         * Such properties lack value storage, so setting them is like writing
         * to /dev/null.
         */
        if (SPROP_HAS_STUB_SETTER(sprop))
            return JS_TRUE;
        pval = JSVAL_VOID;
    }

    sample = cx->runtime->propertyRemovals;
    JS_UNLOCK_SCOPE(cx, scope);
    JS_PUSH_TEMP_ROOT_SPROP(cx, sprop, &tvr);
    ok = SPROP_SET(cx, sprop, obj, obj, vp);
    JS_POP_TEMP_ROOT(cx, &tvr);
    if (!ok)
        return JS_FALSE;

    JS_LOCK_SCOPE(cx, scope);
    JS_ASSERT(scope->object == obj);
    if (SLOT_IN_SCOPE(slot, scope) &&
        (JS_LIKELY(cx->runtime->propertyRemovals == sample) ||
         SCOPE_GET_PROPERTY(scope, sprop->id) == sprop)) {
  set_slot:
        GC_POKE(cx, pval);
        LOCKED_OBJ_SET_SLOT(obj, slot, *vp);
    }

    return JS_TRUE;
}

JSBool
js_GetProperty(JSContext *cx, JSObject *obj, jsid id, jsval *vp)
{
    JSObject *obj2;
    JSProperty *prop;
    JSScopeProperty *sprop;

    /*
     * Handle old bug that took empty string as zero index.  Also convert
     * string indices to integers if appropriate.
     */
    CHECK_FOR_STRING_INDEX(id);

    if (!js_LookupProperty(cx, obj, id, &obj2, &prop))
        return JS_FALSE;
    if (!prop) {
        jsbytecode *pc;

        *vp = JSVAL_VOID;

        if (!OBJ_GET_CLASS(cx, obj)->getProperty(cx, obj, ID_TO_VALUE(id), vp))
            return JS_FALSE;

        /*
         * Give a strict warning if foo.bar is evaluated by a script for an
         * object foo with no property named 'bar'.
         */
        if (JSVAL_IS_VOID(*vp) && cx->fp && (pc = cx->fp->pc)) {
            JSOp op;
            uintN flags;
            JSString *str;

            op = *pc;
            if (op == JSOP_GETXPROP || op == JSOP_GETXELEM) {
                flags = JSREPORT_ERROR;
            } else {
                if (!JS_HAS_STRICT_OPTION(cx) ||
                    (op != JSOP_GETPROP && op != JSOP_GETELEM)) {
                    return JS_TRUE;
                }

                /*
                 * XXX do not warn about missing __iterator__ as the function
                 * may be called from JS_GetMethodById. See bug 355145.
                 */
                if (id == ATOM_TO_JSID(cx->runtime->atomState.iteratorAtom))
                    return JS_TRUE;

                /* Kludge to allow (typeof foo == "undefined") tests. */
                JS_ASSERT(cx->fp->script);
                pc += js_CodeSpec[op].length;
                if (Detecting(cx, pc))
                    return JS_TRUE;

                flags = JSREPORT_WARNING | JSREPORT_STRICT;
            }

            /* Ok, bad undefined property reference: whine about it. */
            str = js_DecompileValueGenerator(cx, JSDVG_IGNORE_STACK,
                                             ID_TO_VALUE(id), NULL);
            if (!str ||
                !JS_ReportErrorFlagsAndNumber(cx, flags,
                                              js_GetErrorMessage, NULL,
                                              JSMSG_UNDEFINED_PROP,
                                              JS_GetStringBytes(str))) {
                return JS_FALSE;
            }
        }
        return JS_TRUE;
    }

    if (!OBJ_IS_NATIVE(obj2)) {
        OBJ_DROP_PROPERTY(cx, obj2, prop);
        return OBJ_GET_PROPERTY(cx, obj2, id, vp);
    }

    sprop = (JSScopeProperty *) prop;
    if (!js_NativeGet(cx, obj, obj2, sprop, vp))
        return JS_FALSE;

    PROPERTY_CACHE_FILL(&cx->runtime->propertyCache, obj2, id, sprop);
    JS_UNLOCK_OBJ(cx, obj2);
    return JS_TRUE;
}

JSBool
js_SetProperty(JSContext *cx, JSObject *obj, jsid id, jsval *vp)
{
    JSObject *pobj;
    JSProperty *prop;
    JSScopeProperty *sprop;
    JSScope *scope;
    uintN attrs, flags;
    intN shortid;
    JSClass *clasp;
    JSPropertyOp getter, setter;

    /*
     * Handle old bug that took empty string as zero index.  Also convert
     * string indices to integers if appropriate.
     */
    CHECK_FOR_STRING_INDEX(id);

    if (!js_LookupProperty(cx, obj, id, &pobj, &prop))
        return JS_FALSE;

    if (prop && !OBJ_IS_NATIVE(pobj)) {
        OBJ_DROP_PROPERTY(cx, pobj, prop);
        prop = NULL;
    }
    sprop = (JSScopeProperty *) prop;

    /*
     * Now either sprop is null, meaning id was not found in obj or one of its
     * prototypes; or sprop is non-null, meaning id was found in pobj's scope.
     * If JS_THREADSAFE and sprop is non-null, then scope is locked, and sprop
     * is held: we must OBJ_DROP_PROPERTY or JS_UNLOCK_SCOPE before we return
     * (the two are equivalent for native objects, but we use JS_UNLOCK_SCOPE
     * because it is cheaper).
     */
    attrs = JSPROP_ENUMERATE;
    flags = 0;
    shortid = 0;
    clasp = OBJ_GET_CLASS(cx, obj);
    getter = clasp->getProperty;
    setter = clasp->setProperty;

    if (sprop) {
        /*
         * Set scope for use below.  It was locked by js_LookupProperty, and
         * we know pobj owns it (i.e., scope->object == pobj).  Therefore we
         * optimize JS_UNLOCK_OBJ(cx, pobj) into JS_UNLOCK_SCOPE(cx, scope).
         */
        scope = OBJ_SCOPE(pobj);

        attrs = sprop->attrs;
        if ((attrs & JSPROP_READONLY) ||
            (SCOPE_IS_SEALED(scope) && pobj == obj)) {
            JS_UNLOCK_SCOPE(cx, scope);

            /*
             * Here, we'll either return true or goto read_only_error, which
             * reports a strict warning or throws an error.  So we redefine
             * the |flags| local variable to be JSREPORT_* flags to pass to
             * JS_ReportErrorFlagsAndNumberUC at label read_only_error.  We
             * must likewise re-task flags further below for the other 'goto
             * read_only_error;' case.
             */
            flags = JSREPORT_ERROR;
            if ((attrs & JSPROP_READONLY) && JS_VERSION_IS_ECMA(cx)) {
                if (!JS_HAS_STRICT_OPTION(cx)) {
                    /* Just return true per ECMA if not in strict mode. */
                    return JS_TRUE;
                }

                /* Strict mode: report a read-only strict warning. */
                flags = JSREPORT_STRICT | JSREPORT_WARNING;
            }
            goto read_only_error;
        }

        if (pobj != obj) {
            /*
             * We found id in a prototype object: prepare to share or shadow.
             * NB: Thanks to the immutable, garbage-collected property tree
             * maintained by jsscope.c in cx->runtime, we needn't worry about
             * sprop going away behind our back after we've unlocked scope.
             */
            JS_UNLOCK_SCOPE(cx, scope);

            /* Don't clone a shared prototype property. */
            if (attrs & JSPROP_SHARED) {
                if (SPROP_HAS_STUB_SETTER(sprop) &&
                    !(sprop->attrs & JSPROP_GETTER)) {
                    return JS_TRUE;
                }
                return SPROP_SET(cx, sprop, obj, pobj, vp);
            }

            /* Restore attrs to the ECMA default for new properties. */
            attrs = JSPROP_ENUMERATE;

            /*
             * Preserve the shortid, getter, and setter when shadowing any
             * property that has a shortid.  An old API convention requires
             * that the property's getter and setter functions receive the
             * shortid, not id, when they are called on the shadow we are
             * about to create in obj's scope.
             */
            if (sprop->flags & SPROP_HAS_SHORTID) {
                flags = SPROP_HAS_SHORTID;
                shortid = sprop->shortid;
                getter = sprop->getter;
                setter = sprop->setter;
            }

            /*
             * Forget we found the proto-property now that we've copied any
             * needed member values.
             */
            sprop = NULL;
        }
#ifdef __GNUC__ /* suppress bogus gcc warnings */
    } else {
        scope = NULL;
#endif
    }

    if (!sprop) {
        if (SCOPE_IS_SEALED(OBJ_SCOPE(obj)) && OBJ_SCOPE(obj)->object == obj) {
            flags = JSREPORT_ERROR;
            goto read_only_error;
        }

        /* Find or make a property descriptor with the right heritage. */
        JS_LOCK_OBJ(cx, obj);
        scope = js_GetMutableScope(cx, obj);
        if (!scope) {
            JS_UNLOCK_OBJ(cx, obj);
            return JS_FALSE;
        }
        if (clasp->flags & JSCLASS_SHARE_ALL_PROPERTIES)
            attrs |= JSPROP_SHARED;
        sprop = js_AddScopeProperty(cx, scope, id, getter, setter,
                                    SPROP_INVALID_SLOT, attrs, flags, shortid);
        if (!sprop) {
            JS_UNLOCK_SCOPE(cx, scope);
            return JS_FALSE;
        }

        /*
         * Initialize the new property value (passed to setter) to undefined.
         * Note that we store before calling addProperty, to match the order
         * in js_DefineNativeProperty.
         */
        if (SPROP_HAS_VALID_SLOT(sprop, scope))
            LOCKED_OBJ_SET_SLOT(obj, sprop->slot, JSVAL_VOID);

        /* XXXbe called with obj locked */
        ADD_PROPERTY_HELPER(cx, clasp, obj, scope, sprop, vp,
                            js_RemoveScopeProperty(cx, scope, id);
                            JS_UNLOCK_SCOPE(cx, scope);
                            return JS_FALSE);

        PROPERTY_CACHE_FILL(&cx->runtime->propertyCache, obj, id, sprop);
    }

    if (!js_NativeSet(cx, obj, sprop, vp))
        return JS_FALSE;
    JS_UNLOCK_SCOPE(cx, scope);
    return JS_TRUE;

  read_only_error: {
    JSString *str = js_DecompileValueGenerator(cx,
                                               JSDVG_IGNORE_STACK,
                                               ID_TO_VALUE(id),
                                               NULL);
    if (!str)
        return JS_FALSE;
    return JS_ReportErrorFlagsAndNumberUC(cx, flags, js_GetErrorMessage,
                                          NULL, JSMSG_READ_ONLY,
                                          JS_GetStringChars(str));
  }
}

JSBool
js_GetAttributes(JSContext *cx, JSObject *obj, jsid id, JSProperty *prop,
                 uintN *attrsp)
{
    JSBool noprop, ok;
    JSScopeProperty *sprop;

    noprop = !prop;
    if (noprop) {
        if (!js_LookupProperty(cx, obj, id, &obj, &prop))
            return JS_FALSE;
        if (!prop) {
            *attrsp = 0;
            return JS_TRUE;
        }
        if (!OBJ_IS_NATIVE(obj)) {
            ok = OBJ_GET_ATTRIBUTES(cx, obj, id, prop, attrsp);
            OBJ_DROP_PROPERTY(cx, obj, prop);
            return ok;
        }
    }
    sprop = (JSScopeProperty *)prop;
    *attrsp = sprop->attrs;
    if (noprop)
        OBJ_DROP_PROPERTY(cx, obj, prop);
    return JS_TRUE;
}

JSBool
js_SetAttributes(JSContext *cx, JSObject *obj, jsid id, JSProperty *prop,
                 uintN *attrsp)
{
    JSBool noprop, ok;
    JSScopeProperty *sprop;

    noprop = !prop;
    if (noprop) {
        if (!js_LookupProperty(cx, obj, id, &obj, &prop))
            return JS_FALSE;
        if (!prop)
            return JS_TRUE;
        if (!OBJ_IS_NATIVE(obj)) {
            ok = OBJ_SET_ATTRIBUTES(cx, obj, id, prop, attrsp);
            OBJ_DROP_PROPERTY(cx, obj, prop);
            return ok;
        }
    }
    sprop = (JSScopeProperty *)prop;
    sprop = js_ChangeNativePropertyAttrs(cx, obj, sprop, *attrsp, 0,
                                         sprop->getter, sprop->setter);
    if (noprop)
        OBJ_DROP_PROPERTY(cx, obj, prop);
    return (sprop != NULL);
}

JSBool
js_DeleteProperty(JSContext *cx, JSObject *obj, jsid id, jsval *rval)
{
    JSObject *proto;
    JSProperty *prop;
    JSScopeProperty *sprop;
    JSString *str;
    JSScope *scope;
    JSBool ok;

    *rval = JSVAL_TRUE;

    /*
     * Handle old bug that took empty string as zero index.  Also convert
     * string indices to integers if appropriate.
     */
    CHECK_FOR_STRING_INDEX(id);

    if (!js_LookupProperty(cx, obj, id, &proto, &prop))
        return JS_FALSE;
    if (!prop || proto != obj) {
        /*
         * If the property was found in a native prototype, check whether it's
         * shared and permanent.  Such a property stands for direct properties
         * in all delegating objects, matching ECMA semantics without bloating
         * each delegating object.
         */
        if (prop) {
            if (OBJ_IS_NATIVE(proto)) {
                sprop = (JSScopeProperty *)prop;
                if (SPROP_IS_SHARED_PERMANENT(sprop))
                    *rval = JSVAL_FALSE;
            }
            OBJ_DROP_PROPERTY(cx, proto, prop);
            if (*rval == JSVAL_FALSE)
                return JS_TRUE;
        }

        /*
         * If no property, or the property comes unshared or impermanent from
         * a prototype, call the class's delProperty hook, passing rval as the
         * result parameter.
         */
        return OBJ_GET_CLASS(cx, obj)->delProperty(cx, obj, ID_TO_VALUE(id),
                                                   rval);
    }

    sprop = (JSScopeProperty *)prop;
    if (sprop->attrs & JSPROP_PERMANENT) {
        OBJ_DROP_PROPERTY(cx, obj, prop);
        if (JS_VERSION_IS_ECMA(cx)) {
            *rval = JSVAL_FALSE;
            return JS_TRUE;
        }
        str = js_DecompileValueGenerator(cx, JSDVG_IGNORE_STACK,
                                         ID_TO_VALUE(id), NULL);
        if (str) {
            JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL,
                                 JSMSG_PERMANENT, JS_GetStringBytes(str));
        }
        return JS_FALSE;
    }

    /* XXXbe called with obj locked */
    if (!LOCKED_OBJ_GET_CLASS(obj)->delProperty(cx, obj, SPROP_USERID(sprop),
                                                rval)) {
        OBJ_DROP_PROPERTY(cx, obj, prop);
        return JS_FALSE;
    }

    scope = OBJ_SCOPE(obj);
    if (SPROP_HAS_VALID_SLOT(sprop, scope))
        GC_POKE(cx, LOCKED_OBJ_GET_SLOT(obj, sprop->slot));

    PROPERTY_CACHE_FILL(&cx->runtime->propertyCache, obj, id, NULL);
    ok = js_RemoveScopeProperty(cx, scope, id);
    OBJ_DROP_PROPERTY(cx, obj, prop);
    return ok;
}

JSBool
js_DefaultValue(JSContext *cx, JSObject *obj, JSType hint, jsval *vp)
{
    jsval v, save;
    JSString *str;

    v = save = OBJECT_TO_JSVAL(obj);
    switch (hint) {
      case JSTYPE_STRING:
        /*
         * Propagate the exception if js_TryMethod finds an appropriate
         * method, and calling that method returned failure.
         */
        if (!js_TryMethod(cx, obj, cx->runtime->atomState.toStringAtom, 0, NULL,
                          &v)) {
            return JS_FALSE;
        }

        if (!JSVAL_IS_PRIMITIVE(v)) {
            if (!OBJ_GET_CLASS(cx, obj)->convert(cx, obj, hint, &v))
                return JS_FALSE;
        }
        break;

      default:
        if (!OBJ_GET_CLASS(cx, obj)->convert(cx, obj, hint, &v))
            return JS_FALSE;
        if (!JSVAL_IS_PRIMITIVE(v)) {
            JSType type = JS_TypeOfValue(cx, v);
            if (type == hint ||
                (type == JSTYPE_FUNCTION && hint == JSTYPE_OBJECT)) {
                goto out;
            }
            if (!js_TryMethod(cx, obj, cx->runtime->atomState.toStringAtom, 0,
                              NULL, &v)) {
                return JS_FALSE;
            }
        }
        break;
    }
    if (!JSVAL_IS_PRIMITIVE(v)) {
        /* Avoid recursive death through js_DecompileValueGenerator. */
        if (hint == JSTYPE_STRING) {
            str = JS_InternString(cx, OBJ_GET_CLASS(cx, obj)->name);
            if (!str)
                return JS_FALSE;
        } else {
            str = NULL;
        }
        *vp = OBJECT_TO_JSVAL(obj);
        str = js_DecompileValueGenerator(cx, JSDVG_SEARCH_STACK, save, str);
        if (str) {
            JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL,
                                 JSMSG_CANT_CONVERT_TO,
                                 JS_GetStringBytes(str),
                                 (hint == JSTYPE_VOID)
                                 ? "primitive type"
                                 : js_type_strs[hint]);
        }
        return JS_FALSE;
    }
out:
    *vp = v;
    return JS_TRUE;
}

JSIdArray *
js_NewIdArray(JSContext *cx, jsint length)
{
    JSIdArray *ida;

    ida = (JSIdArray *)
          JS_malloc(cx, sizeof(JSIdArray) + (length-1) * sizeof(jsval));
    if (ida)
        ida->length = length;
    return ida;
}

JSIdArray *
js_SetIdArrayLength(JSContext *cx, JSIdArray *ida, jsint length)
{
    JSIdArray *rida;

    rida = (JSIdArray *)
           JS_realloc(cx, ida, sizeof(JSIdArray) + (length-1) * sizeof(jsval));
    if (!rida)
        JS_DestroyIdArray(cx, ida);
    else
        rida->length = length;
    return rida;
}

/* Private type used to iterate over all properties of a native JS object */
struct JSNativeIteratorState {
    jsint                   next_index; /* index into jsid array */
    JSIdArray               *ida;       /* all property ids in enumeration */
    JSNativeIteratorState   *next;      /* double-linked list support */
    JSNativeIteratorState   **prevp;
};

/*
 * This function is used to enumerate the properties of native JSObjects
 * and those host objects that do not define a JSNewEnumerateOp-style iterator
 * function.
 */
JSBool
js_Enumerate(JSContext *cx, JSObject *obj, JSIterateOp enum_op,
             jsval *statep, jsid *idp)
{
    JSRuntime *rt;
    JSObject *proto;
    JSClass *clasp;
    JSEnumerateOp enumerate;
    JSScopeProperty *sprop, *lastProp;
    jsint i, length;
    JSScope *scope;
    JSIdArray *ida;
    JSNativeIteratorState *state;

    rt = cx->runtime;
    clasp = OBJ_GET_CLASS(cx, obj);
    enumerate = clasp->enumerate;
    if (clasp->flags & JSCLASS_NEW_ENUMERATE)
        return ((JSNewEnumerateOp) enumerate)(cx, obj, enum_op, statep, idp);

    switch (enum_op) {
      case JSENUMERATE_INIT:
        if (!enumerate(cx, obj))
            return JS_FALSE;
        length = 0;

        /*
         * The set of all property ids is pre-computed when the iterator
         * is initialized so as to avoid problems with properties being
         * deleted during the iteration.
         */
        JS_LOCK_OBJ(cx, obj);
        scope = OBJ_SCOPE(obj);

        /*
         * If this object shares a scope with its prototype, don't enumerate
         * its properties.  Otherwise they will be enumerated a second time
         * when the prototype object is enumerated.
         */
        proto = OBJ_GET_PROTO(cx, obj);
        if (proto && scope == OBJ_SCOPE(proto)) {
            ida = js_NewIdArray(cx, 0);
            if (!ida) {
                JS_UNLOCK_OBJ(cx, obj);
                return JS_FALSE;
            }
        } else {
            /* Object has a private scope; Enumerate all props in scope. */
            for (sprop = lastProp = SCOPE_LAST_PROP(scope); sprop;
                 sprop = sprop->parent) {
                if ((
#ifdef DUMP_CALL_TABLE
                     (cx->options & JSOPTION_LOGCALL_TOSOURCE) ||
#endif
                     (sprop->attrs & JSPROP_ENUMERATE)) &&
                    !(sprop->flags & SPROP_IS_ALIAS) &&
                    (!SCOPE_HAD_MIDDLE_DELETE(scope) ||
                     SCOPE_HAS_PROPERTY(scope, sprop))) {
                    length++;
                }
            }
            ida = js_NewIdArray(cx, length);
            if (!ida) {
                JS_UNLOCK_OBJ(cx, obj);
                return JS_FALSE;
            }
            i = length;
            for (sprop = lastProp; sprop; sprop = sprop->parent) {
                if ((
#ifdef DUMP_CALL_TABLE
                     (cx->options & JSOPTION_LOGCALL_TOSOURCE) ||
#endif
                     (sprop->attrs & JSPROP_ENUMERATE)) &&
                    !(sprop->flags & SPROP_IS_ALIAS) &&
                    (!SCOPE_HAD_MIDDLE_DELETE(scope) ||
                     SCOPE_HAS_PROPERTY(scope, sprop))) {
                    JS_ASSERT(i > 0);
                    ida->vector[--i] = sprop->id;
                }
            }
        }
        JS_UNLOCK_OBJ(cx, obj);

        state = (JSNativeIteratorState *)
            JS_malloc(cx, sizeof(JSNativeIteratorState));
        if (!state) {
            JS_DestroyIdArray(cx, ida);
            return JS_FALSE;
        }
        state->ida = ida;
        state->next_index = 0;

        JS_LOCK_RUNTIME(rt);
        state->next = rt->nativeIteratorStates;
        if (state->next)
            state->next->prevp = &state->next;
        state->prevp = &rt->nativeIteratorStates;
        *state->prevp = state;
        JS_UNLOCK_RUNTIME(rt);

        *statep = PRIVATE_TO_JSVAL(state);
        if (idp)
            *idp = INT_TO_JSVAL(length);
        break;

      case JSENUMERATE_NEXT:
        state = (JSNativeIteratorState *) JSVAL_TO_PRIVATE(*statep);
        ida = state->ida;
        length = ida->length;
        if (state->next_index != length) {
            *idp = ida->vector[state->next_index++];
            break;
        }
        /* FALL THROUGH */

      case JSENUMERATE_DESTROY:
        state = (JSNativeIteratorState *) JSVAL_TO_PRIVATE(*statep);

        JS_LOCK_RUNTIME(rt);
        JS_ASSERT(rt->nativeIteratorStates);
        JS_ASSERT(*state->prevp == state);
        if (state->next) {
            JS_ASSERT(state->next->prevp == &state->next);
            state->next->prevp = state->prevp;
        }
        *state->prevp = state->next;
        JS_UNLOCK_RUNTIME(rt);

        JS_DestroyIdArray(cx, state->ida);
        JS_free(cx, state);
        *statep = JSVAL_NULL;
        break;
    }
    return JS_TRUE;
}

void
js_MarkNativeIteratorStates(JSContext *cx)
{
    JSNativeIteratorState *state;
    jsid *cursor, *end, id;

    state = cx->runtime->nativeIteratorStates;
    if (!state)
        return;

    do {
        JS_ASSERT(*state->prevp == state);
        cursor = state->ida->vector;
        end = cursor + state->ida->length;
        for (; cursor != end; ++cursor) {
            id = *cursor;
            MARK_ID(cx, id);
        }
    } while ((state = state->next) != NULL);
}

JSBool
js_CheckAccess(JSContext *cx, JSObject *obj, jsid id, JSAccessMode mode,
               jsval *vp, uintN *attrsp)
{
    JSBool writing;
    JSObject *pobj;
    JSProperty *prop;
    JSClass *clasp;
    JSScopeProperty *sprop;
    JSCheckAccessOp check;

    writing = (mode & JSACC_WRITE) != 0;
    switch (mode & JSACC_TYPEMASK) {
      case JSACC_PROTO:
        pobj = obj;
        if (!writing)
            *vp = OBJ_GET_SLOT(cx, obj, JSSLOT_PROTO);
        *attrsp = JSPROP_PERMANENT;
        break;

      case JSACC_PARENT:
        JS_ASSERT(!writing);
        pobj = obj;
        *vp = OBJ_GET_SLOT(cx, obj, JSSLOT_PARENT);
        *attrsp = JSPROP_READONLY | JSPROP_PERMANENT;
        break;

      default:
        if (!js_LookupProperty(cx, obj, id, &pobj, &prop))
            return JS_FALSE;
        if (!prop) {
            if (!writing)
                *vp = JSVAL_VOID;
            *attrsp = 0;
            clasp = OBJ_GET_CLASS(cx, obj);
            return !clasp->checkAccess ||
                   clasp->checkAccess(cx, obj, ID_TO_VALUE(id), mode, vp);
        }
        if (!OBJ_IS_NATIVE(pobj)) {
            OBJ_DROP_PROPERTY(cx, pobj, prop);
            return OBJ_CHECK_ACCESS(cx, pobj, id, mode, vp, attrsp);
        }

        sprop = (JSScopeProperty *)prop;
        *attrsp = sprop->attrs;
        if (!writing) {
            *vp = (SPROP_HAS_VALID_SLOT(sprop, OBJ_SCOPE(pobj)))
                  ? LOCKED_OBJ_GET_SLOT(pobj, sprop->slot)
                  : JSVAL_VOID;
        }
        OBJ_DROP_PROPERTY(cx, pobj, prop);
    }

    /*
     * If obj's class has a stub (null) checkAccess hook, use the per-runtime
     * checkObjectAccess callback, if configured.
     *
     * We don't want to require all classes to supply a checkAccess hook; we
     * need that hook only for certain classes used when precompiling scripts
     * and functions ("brutal sharing").  But for general safety of built-in
     * magic properties such as __proto__ and __parent__, we route all access
     * checks, even for classes that stub out checkAccess, through the global
     * checkObjectAccess hook.  This covers precompilation-based sharing and
     * (possibly unintended) runtime sharing across trust boundaries.
     */
    clasp = OBJ_GET_CLASS(cx, pobj);
    check = clasp->checkAccess;
    if (!check)
        check = cx->runtime->checkObjectAccess;
    return !check || check(cx, pobj, ID_TO_VALUE(id), mode, vp);
}

#ifdef JS_THREADSAFE
void
js_DropProperty(JSContext *cx, JSObject *obj, JSProperty *prop)
{
    JS_UNLOCK_OBJ(cx, obj);
}
#endif

static void
ReportIsNotFunction(JSContext *cx, jsval *vp, uintN flags)
{
    /*
     * The decompiler may need to access the args of the function in
     * progress rather than the one we had hoped to call.
     * So we switch the cx->fp to the frame below us. We stick the
     * current frame in the dormantFrameChain to protect it from gc.
     */

    JSStackFrame *fp = cx->fp;
    if (fp->down) {
        JS_ASSERT(!fp->dormantNext);
        fp->dormantNext = cx->dormantFrameChain;
        cx->dormantFrameChain = fp;
        cx->fp = fp->down;
    }

    js_ReportIsNotFunction(cx, vp, flags);

    if (fp->down) {
        JS_ASSERT(cx->dormantFrameChain == fp);
        cx->dormantFrameChain = fp->dormantNext;
        fp->dormantNext = NULL;
        cx->fp = fp;
    }
}

#ifdef NARCISSUS
static JSBool
GetCurrentExecutionContext(JSContext *cx, JSObject *obj, jsval *rval)
{
    JSObject *tmp;
    jsval xcval;

    while ((tmp = OBJ_GET_PARENT(cx, obj)) != NULL)
        obj = tmp;
    if (!OBJ_GET_PROPERTY(cx, obj,
                          ATOM_TO_JSID(cx->runtime->atomState
                                       .ExecutionContextAtom),
                          &xcval)) {
        return JS_FALSE;
    }
    if (JSVAL_IS_PRIMITIVE(xcval)) {
        JS_ReportError(cx, "invalid ExecutionContext in global object");
        return JS_FALSE;
    }
    if (!OBJ_GET_PROPERTY(cx, JSVAL_TO_OBJECT(xcval),
                          ATOM_TO_JSID(cx->runtime->atomState.currentAtom),
                          rval)) {
        return JS_FALSE;
    }
    return JS_TRUE;
}
#endif

JSBool
js_Call(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval)
{
    JSClass *clasp;

    clasp = OBJ_GET_CLASS(cx, JSVAL_TO_OBJECT(argv[-2]));
    if (!clasp->call) {
#ifdef NARCISSUS
        JSObject *callee, *args;
        jsval fval, nargv[3];
        JSBool ok;

        callee = JSVAL_TO_OBJECT(argv[-2]);
        if (!OBJ_GET_PROPERTY(cx, callee,
                              ATOM_TO_JSID(cx->runtime->atomState.callAtom),
                              &fval)) {
            return JS_FALSE;
        }
        if (VALUE_IS_FUNCTION(cx, fval)) {
            if (!GetCurrentExecutionContext(cx, obj, &nargv[2]))
                return JS_FALSE;
            args = js_GetArgsObject(cx, cx->fp);
            if (!args)
                return JS_FALSE;
            nargv[0] = OBJECT_TO_JSVAL(obj);
            nargv[1] = OBJECT_TO_JSVAL(args);
            return js_InternalCall(cx, callee, fval, 3, nargv, rval);
        }
        if (JSVAL_IS_OBJECT(fval) && JSVAL_TO_OBJECT(fval) != callee) {
            argv[-2] = fval;
            ok = js_Call(cx, obj, argc, argv, rval);
            argv[-2] = OBJECT_TO_JSVAL(callee);
            return ok;
        }
#endif
        ReportIsNotFunction(cx, &argv[-2], cx->fp->flags & JSFRAME_ITERATOR);
        return JS_FALSE;
    }
    return clasp->call(cx, obj, argc, argv, rval);
}

JSBool
js_Construct(JSContext *cx, JSObject *obj, uintN argc, jsval *argv,
             jsval *rval)
{
    JSClass *clasp;

    clasp = OBJ_GET_CLASS(cx, JSVAL_TO_OBJECT(argv[-2]));
    if (!clasp->construct) {
#ifdef NARCISSUS
        JSObject *callee, *args;
        jsval cval, nargv[2];
        JSBool ok;

        callee = JSVAL_TO_OBJECT(argv[-2]);
        if (!OBJ_GET_PROPERTY(cx, callee,
                              ATOM_TO_JSID(cx->runtime->atomState
                                           .constructAtom),
                              &cval)) {
            return JS_FALSE;
        }
        if (VALUE_IS_FUNCTION(cx, cval)) {
            if (!GetCurrentExecutionContext(cx, obj, &nargv[1]))
                return JS_FALSE;
            args = js_GetArgsObject(cx, cx->fp);
            if (!args)
                return JS_FALSE;
            nargv[0] = OBJECT_TO_JSVAL(args);
            return js_InternalCall(cx, callee, cval, 2, nargv, rval);
        }
        if (JSVAL_IS_OBJECT(cval) && JSVAL_TO_OBJECT(cval) != callee) {
            argv[-2] = cval;
            ok = js_Call(cx, obj, argc, argv, rval);
            argv[-2] = OBJECT_TO_JSVAL(callee);
            return ok;
        }
#endif
        ReportIsNotFunction(cx, &argv[-2], JSV2F_CONSTRUCT);
        return JS_FALSE;
    }
    return clasp->construct(cx, obj, argc, argv, rval);
}

JSBool
js_HasInstance(JSContext *cx, JSObject *obj, jsval v, JSBool *bp)
{
    JSClass *clasp;
    JSString *str;

    clasp = OBJ_GET_CLASS(cx, obj);
    if (clasp->hasInstance)
        return clasp->hasInstance(cx, obj, v, bp);
#ifdef NARCISSUS
    {
        jsval fval, rval;

        if (!OBJ_GET_PROPERTY(cx, obj,
                              ATOM_TO_JSID(cx->runtime->atomState
                                           .hasInstanceAtom),
                              &fval)) {
            return JS_FALSE;
        }
        if (VALUE_IS_FUNCTION(cx, fval)) {
            return js_InternalCall(cx, obj, fval, 1, &v, &rval) &&
                   js_ValueToBoolean(cx, rval, bp);
        }
    }
#endif
    str = js_DecompileValueGenerator(cx, JSDVG_SEARCH_STACK,
                                     OBJECT_TO_JSVAL(obj), NULL);
    if (str) {
        JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL,
                             JSMSG_BAD_INSTANCEOF_RHS,
                             JS_GetStringBytes(str));
    }
    return JS_FALSE;
}

JSBool
js_IsDelegate(JSContext *cx, JSObject *obj, jsval v, JSBool *bp)
{
    JSObject *obj2;

    *bp = JS_FALSE;
    if (JSVAL_IS_PRIMITIVE(v))
        return JS_TRUE;
    obj2 = JSVAL_TO_OBJECT(v);
    while ((obj2 = OBJ_GET_PROTO(cx, obj2)) != NULL) {
        if (obj2 == obj) {
            *bp = JS_TRUE;
            break;
        }
    }
    return JS_TRUE;
}

JSBool
js_GetClassPrototype(JSContext *cx, JSObject *scope, jsid id,
                     JSObject **protop)
{
    jsval v;
    JSObject *ctor;

    if (!js_FindClassObject(cx, scope, id, &v))
        return JS_FALSE;
    if (VALUE_IS_FUNCTION(cx, v)) {
        ctor = JSVAL_TO_OBJECT(v);
        if (!OBJ_GET_PROPERTY(cx, ctor,
                              ATOM_TO_JSID(cx->runtime->atomState
                                           .classPrototypeAtom),
                              &v)) {
            return JS_FALSE;
        }
        if (!JSVAL_IS_PRIMITIVE(v)) {
            /*
             * Set the newborn root in case v is otherwise unreferenced.
             * It's ok to overwrite newborn roots here, since the getter
             * called just above could have.  Unlike the common GC rooting
             * model, our callers do not have to protect protop thanks to
             * this newborn root, since they all immediately create a new
             * instance that delegates to this object, or just query the
             * prototype for its class.
             */
            cx->weakRoots.newborn[GCX_OBJECT] = JSVAL_TO_GCTHING(v);
        }
    }
    *protop = JSVAL_IS_OBJECT(v) ? JSVAL_TO_OBJECT(v) : NULL;
    return JS_TRUE;
}

/*
 * For shared precompilation of function objects, we support cloning on entry
 * to an execution context in which the function declaration or expression
 * should be processed as if it were not precompiled, where the precompiled
 * function's scope chain does not match the execution context's.  The cloned
 * function object carries its execution-context scope in its parent slot; it
 * links to the precompiled function (the "clone-parent") via its proto slot.
 *
 * Note that this prototype-based delegation leaves an unchecked access path
 * from the clone to the clone-parent's 'constructor' property.  If the clone
 * lives in a less privileged or shared scope than the clone-parent, this is
 * a security hole, a sharing hazard, or both.  Therefore we check all such
 * accesses with the following getter/setter pair, which we use when defining
 * 'constructor' in f.prototype for all function objects f.
 */
static JSBool
CheckCtorGetAccess(JSContext *cx, JSObject *obj, jsval id, jsval *vp)
{
    JSAtom *atom;
    uintN attrs;

    atom = cx->runtime->atomState.constructorAtom;
    JS_ASSERT(id == ATOM_KEY(atom));
    return OBJ_CHECK_ACCESS(cx, obj, ATOM_TO_JSID(atom), JSACC_READ,
                            vp, &attrs);
}

static JSBool
CheckCtorSetAccess(JSContext *cx, JSObject *obj, jsval id, jsval *vp)
{
    JSAtom *atom;
    uintN attrs;

    atom = cx->runtime->atomState.constructorAtom;
    JS_ASSERT(id == ATOM_KEY(atom));
    return OBJ_CHECK_ACCESS(cx, obj, ATOM_TO_JSID(atom), JSACC_WRITE,
                            vp, &attrs);
}

JSBool
js_SetClassPrototype(JSContext *cx, JSObject *ctor, JSObject *proto,
                     uintN attrs)
{
    /*
     * Use the given attributes for the prototype property of the constructor,
     * as user-defined constructors have a DontDelete prototype (which may be
     * reset), while native or "system" constructors have DontEnum | ReadOnly |
     * DontDelete.
     */
    if (!OBJ_DEFINE_PROPERTY(cx, ctor,
                             ATOM_TO_JSID(cx->runtime->atomState
                                          .classPrototypeAtom),
                             OBJECT_TO_JSVAL(proto),
                             JS_PropertyStub, JS_PropertyStub,
                             attrs, NULL)) {
        return JS_FALSE;
    }

    /*
     * ECMA says that Object.prototype.constructor, or f.prototype.constructor
     * for a user-defined function f, is DontEnum.
     */
    return OBJ_DEFINE_PROPERTY(cx, proto,
                               ATOM_TO_JSID(cx->runtime->atomState
                                            .constructorAtom),
                               OBJECT_TO_JSVAL(ctor),
                               CheckCtorGetAccess, CheckCtorSetAccess,
                               0, NULL);
}

JSBool
js_ValueToObject(JSContext *cx, jsval v, JSObject **objp)
{
    JSObject *obj;

    if (JSVAL_IS_NULL(v) || JSVAL_IS_VOID(v)) {
        obj = NULL;
    } else if (JSVAL_IS_OBJECT(v)) {
        obj = JSVAL_TO_OBJECT(v);
        if (!OBJ_DEFAULT_VALUE(cx, obj, JSTYPE_OBJECT, &v))
            return JS_FALSE;
        if (JSVAL_IS_OBJECT(v))
            obj = JSVAL_TO_OBJECT(v);
    } else {
        if (JSVAL_IS_STRING(v)) {
            obj = js_StringToObject(cx, JSVAL_TO_STRING(v));
        } else if (JSVAL_IS_INT(v)) {
            obj = js_NumberToObject(cx, (jsdouble)JSVAL_TO_INT(v));
        } else if (JSVAL_IS_DOUBLE(v)) {
            obj = js_NumberToObject(cx, *JSVAL_TO_DOUBLE(v));
        } else {
            JS_ASSERT(JSVAL_IS_BOOLEAN(v));
            obj = js_BooleanToObject(cx, JSVAL_TO_BOOLEAN(v));
        }
        if (!obj)
            return JS_FALSE;
    }
    *objp = obj;
    return JS_TRUE;
}

JSObject *
js_ValueToNonNullObject(JSContext *cx, jsval v)
{
    JSObject *obj;
    JSString *str;

    if (!js_ValueToObject(cx, v, &obj))
        return NULL;
    if (!obj) {
        str = js_DecompileValueGenerator(cx, JSDVG_SEARCH_STACK, v, NULL);
        if (str) {
            JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL,
                                 JSMSG_NO_PROPERTIES, JS_GetStringBytes(str));
        }
    }
    return obj;
}

JSBool
js_TryValueOf(JSContext *cx, JSObject *obj, JSType type, jsval *rval)
{
    jsval argv[1];

    argv[0] = ATOM_KEY(cx->runtime->atomState.typeAtoms[type]);
    return js_TryMethod(cx, obj, cx->runtime->atomState.valueOfAtom, 1, argv,
                        rval);
}

JSBool
js_TryMethod(JSContext *cx, JSObject *obj, JSAtom *atom,
             uintN argc, jsval *argv, jsval *rval)
{
    JSErrorReporter older;
    jsid id;
    jsval fval;
    JSBool ok;
    int stackDummy;

    if (!JS_CHECK_STACK_SIZE(cx, stackDummy)) {
        JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_OVER_RECURSED);
        return JS_FALSE;
    }

    /*
     * Report failure only if an appropriate method was found, and calling it
     * returned failure.  We propagate failure in this case to make exceptions
     * behave properly.
     */
    older = JS_SetErrorReporter(cx, NULL);
    id = ATOM_TO_JSID(atom);
    fval = JSVAL_VOID;
#if JS_HAS_XML_SUPPORT
    if (OBJECT_IS_XML(cx, obj)) {
        JSXMLObjectOps *ops;

        ops = (JSXMLObjectOps *) obj->map->ops;
        obj = ops->getMethod(cx, obj, id, &fval);
        ok = (obj != NULL);
    } else
#endif
    {
        ok = OBJ_GET_PROPERTY(cx, obj, id, &fval);
    }
    if (!ok)
        JS_ClearPendingException(cx);
    JS_SetErrorReporter(cx, older);

    return JSVAL_IS_PRIMITIVE(fval) ||
           js_InternalCall(cx, obj, fval, argc, argv, rval);
}

#if JS_HAS_XDR

JSBool
js_XDRObject(JSXDRState *xdr, JSObject **objp)
{
    JSContext *cx;
    JSAtom *atom;
    JSClass *clasp;
    uint32 classId, classDef;
    JSProtoKey protoKey;
    jsid classKey;
    JSObject *proto;

    cx = xdr->cx;
    atom = NULL;
    if (xdr->mode == JSXDR_ENCODE) {
        clasp = OBJ_GET_CLASS(cx, *objp);
        classId = JS_XDRFindClassIdByName(xdr, clasp->name);
        classDef = !classId;
        if (classDef) {
            if (!JS_XDRRegisterClass(xdr, clasp, &classId))
                return JS_FALSE;
            protoKey = JSCLASS_CACHED_PROTO_KEY(clasp);
            if (protoKey != JSProto_Null) {
                classDef |= (protoKey << 1);
            } else {
                atom = js_Atomize(cx, clasp->name, strlen(clasp->name), 0);
                if (!atom)
                    return JS_FALSE;
            }
        }
    } else {
        clasp = NULL;           /* quell GCC overwarning */
        classDef = 0;
    }

    /*
     * XDR a flag word, which could be 0 for a class use, in which case no
     * name follows, only the id in xdr's class registry; 1 for a class def,
     * in which case the flag word is followed by the class name transferred
     * from or to atom; or a value greater than 1, an odd number that when
     * divided by two yields the JSProtoKey for class.  In the last case, as
     * in the 0 classDef case, no name is transferred via atom.
     */
    if (!JS_XDRUint32(xdr, &classDef))
        return JS_FALSE;
    if (classDef == 1 && !js_XDRCStringAtom(xdr, &atom))
        return JS_FALSE;

    if (!JS_XDRUint32(xdr, &classId))
        return JS_FALSE;

    if (xdr->mode == JSXDR_DECODE) {
        if (classDef) {
            /* NB: we know that JSProto_Null is 0 here, for backward compat. */
            protoKey = classDef >> 1;
            classKey = (protoKey != JSProto_Null)
                       ? INT_TO_JSID(protoKey)
                       : ATOM_TO_JSID(atom);
            if (!js_GetClassPrototype(cx, NULL, classKey, &proto))
                return JS_FALSE;
            clasp = OBJ_GET_CLASS(cx, proto);
            if (!JS_XDRRegisterClass(xdr, clasp, &classId))
                return JS_FALSE;
        } else {
            clasp = JS_XDRFindClassById(xdr, classId);
            if (!clasp) {
                char numBuf[12];
                JS_snprintf(numBuf, sizeof numBuf, "%ld", (long)classId);
                JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL,
                                     JSMSG_CANT_FIND_CLASS, numBuf);
                return JS_FALSE;
            }
        }
    }

    if (!clasp->xdrObject) {
        JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL,
                             JSMSG_CANT_XDR_CLASS, clasp->name);
        return JS_FALSE;
    }
    return clasp->xdrObject(xdr, objp);
}

#endif /* JS_HAS_XDR */

#ifdef DEBUG_brendan

#include <stdio.h>
#include <math.h>

uint32 js_entry_count_max;
uint32 js_entry_count_sum;
double js_entry_count_sqsum;
uint32 js_entry_count_hist[11];

static void
MeterEntryCount(uintN count)
{
    if (count) {
        js_entry_count_sum += count;
        js_entry_count_sqsum += (double)count * count;
        if (count > js_entry_count_max)
            js_entry_count_max = count;
    }
    js_entry_count_hist[JS_MIN(count, 10)]++;
}

#define DEBUG_scopemeters
#endif /* DEBUG_brendan */

#ifdef DEBUG_scopemeters
void
js_DumpScopeMeters(JSRuntime *rt)
{
    static FILE *logfp;
    if (!logfp)
        logfp = fopen("/tmp/scope.stats", "a");

    {
        double mean = 0., var = 0., sigma = 0.;
        double nscopes = rt->liveScopes;
        double nentrys = js_entry_count_sum;
        if (nscopes > 0 && nentrys >= 0) {
            mean = nentrys / nscopes;
            var = nscopes * js_entry_count_sqsum - nentrys * nentrys;
            if (var < 0.0 || nscopes <= 1)
                var = 0.0;
            else
                var /= nscopes * (nscopes - 1);

            /* Windows says sqrt(0.0) is "-1.#J" (?!) so we must test. */
            sigma = (var != 0.) ? sqrt(var) : 0.;
        }

        fprintf(logfp,
                "scopes %g entries %g mean %g sigma %g max %u",
                nscopes, nentrys, mean, sigma, js_entry_count_max);
    }

    fprintf(logfp, " histogram %u %u %u %u %u %u %u %u %u %u %u\n",
            js_entry_count_hist[0], js_entry_count_hist[1],
            js_entry_count_hist[2], js_entry_count_hist[3],
            js_entry_count_hist[4], js_entry_count_hist[5],
            js_entry_count_hist[6], js_entry_count_hist[7],
            js_entry_count_hist[8], js_entry_count_hist[9],
            js_entry_count_hist[10]);
    js_entry_count_sum = js_entry_count_max = 0;
    js_entry_count_sqsum = 0;
    memset(js_entry_count_hist, 0, sizeof js_entry_count_hist);
    fflush(logfp);
}
#endif

uint32
js_Mark(JSContext *cx, JSObject *obj, void *arg)
{
    JSScope *scope;
    JSScopeProperty *sprop;
    JSClass *clasp;

    JS_ASSERT(OBJ_IS_NATIVE(obj));
    scope = OBJ_SCOPE(obj);
#ifdef DEBUG_brendan
    if (scope->object == obj)
        MeterEntryCount(scope->entryCount);
#endif

    JS_ASSERT(!SCOPE_LAST_PROP(scope) ||
              SCOPE_HAS_PROPERTY(scope, SCOPE_LAST_PROP(scope)));

    for (sprop = SCOPE_LAST_PROP(scope); sprop; sprop = sprop->parent) {
        if (SCOPE_HAD_MIDDLE_DELETE(scope) && !SCOPE_HAS_PROPERTY(scope, sprop))
            continue;
        MARK_SCOPE_PROPERTY(cx, sprop);
    }

    /* No one runs while the GC is running, so we can use LOCKED_... here. */
    clasp = LOCKED_OBJ_GET_CLASS(obj);
    if (clasp->mark)
        (void) clasp->mark(cx, obj, NULL);

    if (scope->object != obj) {
        /*
         * An unmutated object that shares a prototype's scope.  We can't tell
         * how many slots are allocated and in use at obj->slots by looking at
         * scope, so we get obj->slots' length from its -1'st element.
         */
        return (uint32) obj->slots[-1];
    }
    return JS_MIN(scope->map.freeslot, scope->map.nslots);
}

void
js_Clear(JSContext *cx, JSObject *obj)
{
    JSScope *scope;
    JSRuntime *rt;
    JSScopeProperty *sprop;
    uint32 i, n;

    /*
     * Clear our scope and the property cache of all obj's properties only if
     * obj owns the scope (i.e., not if obj is unmutated and therefore sharing
     * its prototype's scope).  NB: we do not clear any reserved slots lying
     * below JSSLOT_FREE(clasp).
     */
    JS_LOCK_OBJ(cx, obj);
    scope = OBJ_SCOPE(obj);
    if (scope->object == obj) {
        /* Clear the property cache before we clear the scope. */
        rt = cx->runtime;
        for (sprop = SCOPE_LAST_PROP(scope); sprop; sprop = sprop->parent) {
            if (!SCOPE_HAD_MIDDLE_DELETE(scope) ||
                SCOPE_HAS_PROPERTY(scope, sprop)) {
                PROPERTY_CACHE_FILL(&rt->propertyCache, obj, sprop->id, NULL);
            }
        }

        /* Now that we're done using scope->lastProp/table, clear scope. */
        js_ClearScope(cx, scope);

        /* Clear slot values and reset freeslot so we're consistent. */
        i = scope->map.nslots;
        n = JSSLOT_FREE(LOCKED_OBJ_GET_CLASS(obj));
        while (--i >= n)
            obj->slots[i] = JSVAL_VOID;
        scope->map.freeslot = n;
    }
    JS_UNLOCK_OBJ(cx, obj);
}

jsval
js_GetRequiredSlot(JSContext *cx, JSObject *obj, uint32 slot)
{
    jsval v;

    JS_LOCK_OBJ(cx, obj);
    v = (slot < (uint32) obj->slots[-1]) ? obj->slots[slot] : JSVAL_VOID;
    JS_UNLOCK_OBJ(cx, obj);
    return v;
}

JSBool
js_SetRequiredSlot(JSContext *cx, JSObject *obj, uint32 slot, jsval v)
{
    JSScope *scope;
    uint32 nslots;
    JSClass *clasp;
    jsval *newslots;

    JS_LOCK_OBJ(cx, obj);
    scope = OBJ_SCOPE(obj);
    nslots = (uint32) obj->slots[-1];
    if (slot >= nslots) {
        /*
         * At this point, obj may or may not own scope.  If some path calls
         * js_GetMutableScope but does not add a slot-owning property, then
         * scope->object == obj but nslots will be nominal.  If obj shares a
         * prototype's scope, then we cannot update scope->map here, but we
         * must update obj->slots[-1] when we grow obj->slots.
         *
         * See js_Mark, before the last return, where we make a special case
         * for unmutated (scope->object != obj) objects.
         */
        JS_ASSERT(nslots == JS_INITIAL_NSLOTS);
        clasp = LOCKED_OBJ_GET_CLASS(obj);
        nslots = JSSLOT_FREE(clasp);
        if (clasp->reserveSlots)
            nslots += clasp->reserveSlots(cx, obj);
        JS_ASSERT(slot < nslots);

        newslots = AllocSlots(cx, obj->slots, nslots);
        if (!newslots) {
            JS_UNLOCK_SCOPE(cx, scope);
            return JS_FALSE;
        }
        if (scope->object == obj)
            scope->map.nslots = nslots;
        obj->slots = newslots;
    }

    /* Whether or not we grew nslots, we may need to advance freeslot. */
    if (scope->object == obj && slot >= scope->map.freeslot)
        scope->map.freeslot = slot + 1;

    obj->slots[slot] = v;
    JS_UNLOCK_SCOPE(cx, scope);
    return JS_TRUE;
}

#ifdef DEBUG

/* Routines to print out values during debugging. */

void printChar(jschar *cp) {
    fprintf(stderr, "jschar* (0x%p) \"", (void *)cp);
    while (*cp)
        fputc(*cp++, stderr);
    fputc('"', stderr);
    fputc('\n', stderr);
}

void printString(JSString *str) {
    size_t i, n;
    jschar *s;
    fprintf(stderr, "string (0x%p) \"", (void *)str);
    s = JSSTRING_CHARS(str);
    for (i=0, n=JSSTRING_LENGTH(str); i < n; i++)
        fputc(s[i], stderr);
    fputc('"', stderr);
    fputc('\n', stderr);
}

void printVal(JSContext *cx, jsval val);

void printObj(JSContext *cx, JSObject *jsobj) {
    jsuint i;
    jsval val;
    JSClass *clasp;

    fprintf(stderr, "object 0x%p\n", (void *)jsobj);
    clasp = OBJ_GET_CLASS(cx, jsobj);
    fprintf(stderr, "class 0x%p %s\n", (void *)clasp, clasp->name);
    for (i=0; i < jsobj->map->nslots; i++) {
        fprintf(stderr, "slot %3d ", i);
        val = jsobj->slots[i];
        if (JSVAL_IS_OBJECT(val))
            fprintf(stderr, "object 0x%p\n", (void *)JSVAL_TO_OBJECT(val));
        else
            printVal(cx, val);
    }
}

void printVal(JSContext *cx, jsval val) {
    fprintf(stderr, "val %d (0x%p) = ", (int)val, (void *)val);
    if (JSVAL_IS_NULL(val)) {
        fprintf(stderr, "null\n");
    } else if (JSVAL_IS_VOID(val)) {
        fprintf(stderr, "undefined\n");
    } else if (JSVAL_IS_OBJECT(val)) {
        printObj(cx, JSVAL_TO_OBJECT(val));
    } else if (JSVAL_IS_INT(val)) {
        fprintf(stderr, "(int) %d\n", JSVAL_TO_INT(val));
    } else if (JSVAL_IS_STRING(val)) {
        printString(JSVAL_TO_STRING(val));
    } else if (JSVAL_IS_DOUBLE(val)) {
        fprintf(stderr, "(double) %g\n", *JSVAL_TO_DOUBLE(val));
    } else {
        JS_ASSERT(JSVAL_IS_BOOLEAN(val));
        fprintf(stderr, "(boolean) %s\n",
                JSVAL_TO_BOOLEAN(val) ? "true" : "false");
    }
    fflush(stderr);
}

void printId(JSContext *cx, jsid id) {
    fprintf(stderr, "id %d (0x%p) is ", (int)id, (void *)id);
    printVal(cx, ID_TO_VALUE(id));
}

void printAtom(JSAtom *atom) {
    printString(ATOM_TO_STRING(atom));
}

#endif
