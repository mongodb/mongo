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

/*
 * JS atom table.
 */
#include "jsstddef.h"
#include <stdlib.h>
#include <string.h>
#include "jstypes.h"
#include "jsutil.h" /* Added by JSIFY */
#include "jshash.h" /* Added by JSIFY */
#include "jsprf.h"
#include "jsapi.h"
#include "jsatom.h"
#include "jscntxt.h"
#include "jsconfig.h"
#include "jsgc.h"
#include "jslock.h"
#include "jsnum.h"
#include "jsscan.h"
#include "jsstr.h"

JS_FRIEND_API(const char *)
js_AtomToPrintableString(JSContext *cx, JSAtom *atom)
{
    return js_ValueToPrintableString(cx, ATOM_KEY(atom));
}

/*
 * Keep this in sync with jspubtd.h -- an assertion below will insist that
 * its length match the JSType enum's JSTYPE_LIMIT limit value.
 */
const char *js_type_strs[] = {
    "undefined",
    js_object_str,
    "function",
    "string",
    "number",
    "boolean",
    "null",
    "xml",
};

JS_STATIC_ASSERT(JSTYPE_LIMIT ==
                 sizeof js_type_strs / sizeof js_type_strs[0]);

const char *js_boolean_strs[] = {
    js_false_str,
    js_true_str
};

#define JS_PROTO(name,code,init) const char js_##name##_str[] = #name;
#include "jsproto.tbl"
#undef JS_PROTO

const char *js_proto_strs[JSProto_LIMIT] = {
#define JS_PROTO(name,code,init) js_##name##_str,
#include "jsproto.tbl"
#undef JS_PROTO
};

const char js_anonymous_str[]       = "anonymous";
const char js_arguments_str[]       = "arguments";
const char js_arity_str[]           = "arity";
const char js_callee_str[]          = "callee";
const char js_caller_str[]          = "caller";
const char js_class_prototype_str[] = "prototype";
const char js_constructor_str[]     = "constructor";
const char js_count_str[]           = "__count__";
const char js_each_str[]            = "each";
const char js_eval_str[]            = "eval";
const char js_fileName_str[]        = "fileName";
const char js_get_str[]             = "get";
const char js_getter_str[]          = "getter";
const char js_index_str[]           = "index";
const char js_input_str[]           = "input";
const char js_iterator_str[]        = "__iterator__";
const char js_length_str[]          = "length";
const char js_lineNumber_str[]      = "lineNumber";
const char js_message_str[]         = "message";
const char js_name_str[]            = "name";
const char js_next_str[]            = "next";
const char js_noSuchMethod_str[]    = "__noSuchMethod__";
const char js_object_str[]          = "object";
const char js_parent_str[]          = "__parent__";
const char js_proto_str[]           = "__proto__";
const char js_setter_str[]          = "setter";
const char js_set_str[]             = "set";
const char js_stack_str[]           = "stack";
const char js_toSource_str[]        = "toSource";
const char js_toString_str[]        = "toString";
const char js_toLocaleString_str[]  = "toLocaleString";
const char js_valueOf_str[]         = "valueOf";

#if JS_HAS_XML_SUPPORT
const char js_etago_str[]           = "</";
const char js_namespace_str[]       = "namespace";
const char js_ptagc_str[]           = "/>";
const char js_qualifier_str[]       = "::";
const char js_space_str[]           = " ";
const char js_stago_str[]           = "<";
const char js_star_str[]            = "*";
const char js_starQualifier_str[]   = "*::";
const char js_tagc_str[]            = ">";
const char js_xml_str[]             = "xml";
#endif

#if JS_HAS_GENERATORS
const char js_close_str[]           = "close";
const char js_send_str[]            = "send";
#endif

#ifdef NARCISSUS
const char js_call_str[]             = "__call__";
const char js_construct_str[]        = "__construct__";
const char js_hasInstance_str[]      = "__hasInstance__";
const char js_ExecutionContext_str[] = "ExecutionContext";
const char js_current_str[]          = "current";
#endif

#define HASH_OBJECT(o)  (JS_PTR_TO_UINT32(o) >> JSVAL_TAGBITS)
#define HASH_INT(i)     ((JSHashNumber)(i))
#define HASH_DOUBLE(dp) ((JSDOUBLE_HI32(*dp) ^ JSDOUBLE_LO32(*dp)))
#define HASH_BOOLEAN(b) ((JSHashNumber)(b))

JS_STATIC_DLL_CALLBACK(JSHashNumber)
js_hash_atom_key(const void *key)
{
    jsval v;
    jsdouble *dp;

    /* Order JSVAL_IS_* tests by likelihood of success. */
    v = (jsval)key;
    if (JSVAL_IS_STRING(v))
        return js_HashString(JSVAL_TO_STRING(v));
    if (JSVAL_IS_INT(v))
        return HASH_INT(JSVAL_TO_INT(v));
    if (JSVAL_IS_DOUBLE(v)) {
        dp = JSVAL_TO_DOUBLE(v);
        return HASH_DOUBLE(dp);
    }
    if (JSVAL_IS_OBJECT(v))
        return HASH_OBJECT(JSVAL_TO_OBJECT(v));
    if (JSVAL_IS_BOOLEAN(v))
        return HASH_BOOLEAN(JSVAL_TO_BOOLEAN(v));
    return (JSHashNumber)v;
}

JS_STATIC_DLL_CALLBACK(intN)
js_compare_atom_keys(const void *k1, const void *k2)
{
    jsval v1, v2;

    v1 = (jsval)k1, v2 = (jsval)k2;
    if (JSVAL_IS_STRING(v1) && JSVAL_IS_STRING(v2))
        return js_EqualStrings(JSVAL_TO_STRING(v1), JSVAL_TO_STRING(v2));
    if (JSVAL_IS_DOUBLE(v1) && JSVAL_IS_DOUBLE(v2)) {
        double d1 = *JSVAL_TO_DOUBLE(v1);
        double d2 = *JSVAL_TO_DOUBLE(v2);
        if (JSDOUBLE_IS_NaN(d1))
            return JSDOUBLE_IS_NaN(d2);
#if defined(XP_WIN)
        /* XXX MSVC miscompiles such that (NaN == 0) */
        if (JSDOUBLE_IS_NaN(d2))
            return JS_FALSE;
#endif
        return d1 == d2;
    }
    return v1 == v2;
}

JS_STATIC_DLL_CALLBACK(int)
js_compare_stub(const void *v1, const void *v2)
{
    return 1;
}

/* These next two are exported to jsscript.c and used similarly there. */
void * JS_DLL_CALLBACK
js_alloc_table_space(void *priv, size_t size)
{
    return malloc(size);
}

void JS_DLL_CALLBACK
js_free_table_space(void *priv, void *item)
{
    free(item);
}

JS_STATIC_DLL_CALLBACK(JSHashEntry *)
js_alloc_atom(void *priv, const void *key)
{
    JSAtomState *state = (JSAtomState *) priv;
    JSAtom *atom;

    atom = (JSAtom *) malloc(sizeof(JSAtom));
    if (!atom)
        return NULL;
#ifdef JS_THREADSAFE
    state->tablegen++;
#endif
    atom->entry.key = key;
    atom->entry.value = NULL;
    atom->flags = 0;
    atom->number = state->number++;
    return &atom->entry;
}

JS_STATIC_DLL_CALLBACK(void)
js_free_atom(void *priv, JSHashEntry *he, uintN flag)
{
    if (flag != HT_FREE_ENTRY)
        return;
#ifdef JS_THREADSAFE
    ((JSAtomState *)priv)->tablegen++;
#endif
    free(he);
}

static JSHashAllocOps atom_alloc_ops = {
    js_alloc_table_space,   js_free_table_space,
    js_alloc_atom,          js_free_atom
};

#define JS_ATOM_HASH_SIZE   1024

JSBool
js_InitAtomState(JSContext *cx, JSAtomState *state)
{
    state->table = JS_NewHashTable(JS_ATOM_HASH_SIZE, js_hash_atom_key,
                                   js_compare_atom_keys, js_compare_stub,
                                   &atom_alloc_ops, state);
    if (!state->table) {
        JS_ReportOutOfMemory(cx);
        return JS_FALSE;
    }

    state->runtime = cx->runtime;
#ifdef JS_THREADSAFE
    js_InitLock(&state->lock);
    state->tablegen = 0;
#endif

    if (!js_InitPinnedAtoms(cx, state)) {
        js_FreeAtomState(cx, state);
        return JS_FALSE;
    }
    return JS_TRUE;
}

JSBool
js_InitPinnedAtoms(JSContext *cx, JSAtomState *state)
{
    uintN i;

#define FROB(lval,str)                                                        \
    JS_BEGIN_MACRO                                                            \
        if (!(state->lval = js_Atomize(cx, str, strlen(str), ATOM_PINNED)))   \
            return JS_FALSE;                                                  \
    JS_END_MACRO

    for (i = 0; i < JSTYPE_LIMIT; i++)
        FROB(typeAtoms[i],        js_type_strs[i]);

    for (i = 0; i < JSProto_LIMIT; i++)
        FROB(classAtoms[i],       js_proto_strs[i]);

    FROB(booleanAtoms[0],         js_false_str);
    FROB(booleanAtoms[1],         js_true_str);
    FROB(nullAtom,                js_null_str);

    FROB(anonymousAtom,           js_anonymous_str);
    FROB(argumentsAtom,           js_arguments_str);
    FROB(arityAtom,               js_arity_str);
    FROB(calleeAtom,              js_callee_str);
    FROB(callerAtom,              js_caller_str);
    FROB(classPrototypeAtom,      js_class_prototype_str);
    FROB(constructorAtom,         js_constructor_str);
    FROB(countAtom,               js_count_str);
    FROB(eachAtom,                js_each_str);
    FROB(evalAtom,                js_eval_str);
    FROB(fileNameAtom,            js_fileName_str);
    FROB(getAtom,                 js_get_str);
    FROB(getterAtom,              js_getter_str);
    FROB(indexAtom,               js_index_str);
    FROB(inputAtom,               js_input_str);
    FROB(iteratorAtom,            js_iterator_str);
    FROB(lengthAtom,              js_length_str);
    FROB(lineNumberAtom,          js_lineNumber_str);
    FROB(messageAtom,             js_message_str);
    FROB(nameAtom,                js_name_str);
    FROB(nextAtom,                js_next_str);
    FROB(noSuchMethodAtom,        js_noSuchMethod_str);
    FROB(parentAtom,              js_parent_str);
    FROB(protoAtom,               js_proto_str);
    FROB(setAtom,                 js_set_str);
    FROB(setterAtom,              js_setter_str);
    FROB(stackAtom,               js_stack_str);
    FROB(toSourceAtom,            js_toSource_str);
    FROB(toStringAtom,            js_toString_str);
    FROB(toLocaleStringAtom,      js_toLocaleString_str);
    FROB(valueOfAtom,             js_valueOf_str);

#if JS_HAS_XML_SUPPORT
    FROB(etagoAtom,               js_etago_str);
    FROB(namespaceAtom,           js_namespace_str);
    FROB(ptagcAtom,               js_ptagc_str);
    FROB(qualifierAtom,           js_qualifier_str);
    FROB(spaceAtom,               js_space_str);
    FROB(stagoAtom,               js_stago_str);
    FROB(starAtom,                js_star_str);
    FROB(starQualifierAtom,       js_starQualifier_str);
    FROB(tagcAtom,                js_tagc_str);
    FROB(xmlAtom,                 js_xml_str);
#endif

#if JS_HAS_GENERATORS
    FROB(closeAtom,               js_close_str);
#endif

#ifdef NARCISSUS
    FROB(callAtom,                js_call_str);
    FROB(constructAtom,           js_construct_str);
    FROB(hasInstanceAtom,         js_hasInstance_str);
    FROB(ExecutionContextAtom,    js_ExecutionContext_str);
    FROB(currentAtom,             js_current_str);
#endif

#undef FROB

    memset(&state->lazy, 0, sizeof state->lazy);
    return JS_TRUE;
}

/* NB: cx unused; js_FinishAtomState calls us with null cx. */
void
js_FreeAtomState(JSContext *cx, JSAtomState *state)
{
    if (state->table)
        JS_HashTableDestroy(state->table);
#ifdef JS_THREADSAFE
    js_FinishLock(&state->lock);
#endif
    memset(state, 0, sizeof *state);
}

typedef struct UninternArgs {
    JSRuntime   *rt;
    jsatomid    leaks;
} UninternArgs;

JS_STATIC_DLL_CALLBACK(intN)
js_atom_uninterner(JSHashEntry *he, intN i, void *arg)
{
    JSAtom *atom;
    UninternArgs *args;

    atom = (JSAtom *)he;
    args = (UninternArgs *)arg;
    if (ATOM_IS_STRING(atom))
        js_FinalizeStringRT(args->rt, ATOM_TO_STRING(atom));
    else if (ATOM_IS_OBJECT(atom))
        args->leaks++;
    return HT_ENUMERATE_NEXT;
}

void
js_FinishAtomState(JSAtomState *state)
{
    UninternArgs args;

    if (!state->table)
        return;
    args.rt = state->runtime;
    args.leaks = 0;
    JS_HashTableEnumerateEntries(state->table, js_atom_uninterner, &args);
#ifdef DEBUG
    if (args.leaks != 0) {
        fprintf(stderr,
"JS engine warning: %lu atoms remain after destroying the JSRuntime.\n"
"                   These atoms may point to freed memory. Things reachable\n"
"                   through them have not been finalized.\n",
                (unsigned long) args.leaks);
    }
#endif
    js_FreeAtomState(NULL, state);
}

typedef struct MarkArgs {
    JSBool          keepAtoms;
    JSGCThingMarker mark;
    void            *data;
} MarkArgs;

JS_STATIC_DLL_CALLBACK(intN)
js_atom_marker(JSHashEntry *he, intN i, void *arg)
{
    JSAtom *atom;
    MarkArgs *args;
    jsval key;

    atom = (JSAtom *)he;
    args = (MarkArgs *)arg;
    if ((atom->flags & (ATOM_PINNED | ATOM_INTERNED)) || args->keepAtoms) {
        atom->flags |= ATOM_MARK;
        key = ATOM_KEY(atom);
        if (JSVAL_IS_GCTHING(key))
            args->mark(JSVAL_TO_GCTHING(key), args->data);
    }
    return HT_ENUMERATE_NEXT;
}

void
js_MarkAtomState(JSAtomState *state, JSBool keepAtoms, JSGCThingMarker mark,
                 void *data)
{
    MarkArgs args;

    if (!state->table)
        return;
    args.keepAtoms = keepAtoms;
    args.mark = mark;
    args.data = data;
    JS_HashTableEnumerateEntries(state->table, js_atom_marker, &args);
}

JS_STATIC_DLL_CALLBACK(intN)
js_atom_sweeper(JSHashEntry *he, intN i, void *arg)
{
    JSAtom *atom;
    JSAtomState *state;

    atom = (JSAtom *)he;
    if (atom->flags & ATOM_MARK) {
        atom->flags &= ~ATOM_MARK;
        state = (JSAtomState *)arg;
        state->liveAtoms++;
        return HT_ENUMERATE_NEXT;
    }
    JS_ASSERT((atom->flags & (ATOM_PINNED | ATOM_INTERNED)) == 0);
    atom->entry.key = atom->entry.value = NULL;
    atom->flags = 0;
    return HT_ENUMERATE_REMOVE;
}

void
js_SweepAtomState(JSAtomState *state)
{
    state->liveAtoms = 0;
    if (state->table)
        JS_HashTableEnumerateEntries(state->table, js_atom_sweeper, state);
}

JS_STATIC_DLL_CALLBACK(intN)
js_atom_unpinner(JSHashEntry *he, intN i, void *arg)
{
    JSAtom *atom;

    atom = (JSAtom *)he;
    atom->flags &= ~ATOM_PINNED;
    return HT_ENUMERATE_NEXT;
}

void
js_UnpinPinnedAtoms(JSAtomState *state)
{
    if (state->table)
        JS_HashTableEnumerateEntries(state->table, js_atom_unpinner, NULL);
}

static JSAtom *
js_AtomizeHashedKey(JSContext *cx, jsval key, JSHashNumber keyHash, uintN flags)
{
    JSAtomState *state;
    JSHashTable *table;
    JSHashEntry *he, **hep;
    JSAtom *atom;

    state = &cx->runtime->atomState;
    JS_LOCK(&state->lock, cx);
    table = state->table;
    hep = JS_HashTableRawLookup(table, keyHash, (void *)key);
    if ((he = *hep) == NULL) {
        he = JS_HashTableRawAdd(table, hep, keyHash, (void *)key, NULL);
        if (!he) {
            JS_ReportOutOfMemory(cx);
            atom = NULL;
            goto out;
        }
    }

    atom = (JSAtom *)he;
    atom->flags |= flags;
    cx->weakRoots.lastAtom = atom;
out:
    JS_UNLOCK(&state->lock,cx);
    return atom;
}

JSAtom *
js_AtomizeObject(JSContext *cx, JSObject *obj, uintN flags)
{
    jsval key;
    JSHashNumber keyHash;

    /* XXX must be set in the following order or MSVC1.52 will crash */
    keyHash = HASH_OBJECT(obj);
    key = OBJECT_TO_JSVAL(obj);
    return js_AtomizeHashedKey(cx, key, keyHash, flags);
}

JSAtom *
js_AtomizeBoolean(JSContext *cx, JSBool b, uintN flags)
{
    jsval key;
    JSHashNumber keyHash;

    key = BOOLEAN_TO_JSVAL(b);
    keyHash = HASH_BOOLEAN(b);
    return js_AtomizeHashedKey(cx, key, keyHash, flags);
}

JSAtom *
js_AtomizeInt(JSContext *cx, jsint i, uintN flags)
{
    jsval key;
    JSHashNumber keyHash;

    key = INT_TO_JSVAL(i);
    keyHash = HASH_INT(i);
    return js_AtomizeHashedKey(cx, key, keyHash, flags);
}

/* Worst-case alignment grain and aligning macro for 2x-sized buffer. */
#define ALIGNMENT(t)    JS_MAX(JSVAL_ALIGN, sizeof(t))
#define ALIGN(b,t)      ((t*) &(b)[ALIGNMENT(t) - (jsuword)(b) % ALIGNMENT(t)])

JSAtom *
js_AtomizeDouble(JSContext *cx, jsdouble d, uintN flags)
{
    jsdouble *dp;
    JSHashNumber keyHash;
    jsval key;
    JSAtomState *state;
    JSHashTable *table;
    JSHashEntry *he, **hep;
    JSAtom *atom;
    char buf[2 * ALIGNMENT(double)];

    dp = ALIGN(buf, double);
    *dp = d;
    keyHash = HASH_DOUBLE(dp);
    key = DOUBLE_TO_JSVAL(dp);
    state = &cx->runtime->atomState;
    JS_LOCK(&state->lock, cx);
    table = state->table;
    hep = JS_HashTableRawLookup(table, keyHash, (void *)key);
    if ((he = *hep) == NULL) {
#ifdef JS_THREADSAFE
        uint32 gen = state->tablegen;
#endif
        JS_UNLOCK(&state->lock,cx);
        if (!js_NewDoubleValue(cx, d, &key))
            return NULL;
        JS_LOCK(&state->lock, cx);
#ifdef JS_THREADSAFE
        if (state->tablegen != gen) {
            hep = JS_HashTableRawLookup(table, keyHash, (void *)key);
            if ((he = *hep) != NULL) {
                atom = (JSAtom *)he;
                goto out;
            }
        }
#endif
        he = JS_HashTableRawAdd(table, hep, keyHash, (void *)key, NULL);
        if (!he) {
            JS_ReportOutOfMemory(cx);
            atom = NULL;
            goto out;
        }
    }

    atom = (JSAtom *)he;
    atom->flags |= flags;
    cx->weakRoots.lastAtom = atom;
out:
    JS_UNLOCK(&state->lock,cx);
    return atom;
}

/*
 * To put an atom into the hidden subspace. XOR its keyHash with this value,
 * which is (sqrt(2)-1) in 32-bit fixed point.
 */
#define HIDDEN_ATOM_SUBSPACE_KEYHASH    0x6A09E667

JSAtom *
js_AtomizeString(JSContext *cx, JSString *str, uintN flags)
{
    JSHashNumber keyHash;
    jsval key;
    JSAtomState *state;
    JSHashTable *table;
    JSHashEntry *he, **hep;
    JSAtom *atom;

    keyHash = js_HashString(str);
    if (flags & ATOM_HIDDEN)
        keyHash ^= HIDDEN_ATOM_SUBSPACE_KEYHASH;
    key = STRING_TO_JSVAL(str);
    state = &cx->runtime->atomState;
    JS_LOCK(&state->lock, cx);
    table = state->table;
    hep = JS_HashTableRawLookup(table, keyHash, (void *)key);
    if ((he = *hep) == NULL) {
#ifdef JS_THREADSAFE
        uint32 gen = state->tablegen;
        JS_UNLOCK(&state->lock, cx);
#endif

        if (flags & ATOM_TMPSTR) {
            str = (flags & ATOM_NOCOPY)
                  ? js_NewString(cx, str->chars, str->length, 0)
                  : js_NewStringCopyN(cx, str->chars, str->length, 0);
            if (!str)
                return NULL;
            key = STRING_TO_JSVAL(str);
        } else {
            if (!JS_MakeStringImmutable(cx, str))
                return NULL;
        }

#ifdef JS_THREADSAFE
        JS_LOCK(&state->lock, cx);
        if (state->tablegen != gen) {
            hep = JS_HashTableRawLookup(table, keyHash, (void *)key);
            if ((he = *hep) != NULL) {
                atom = (JSAtom *)he;
                if (flags & ATOM_NOCOPY)
                    str->chars = NULL;
                goto out;
            }
        }
#endif

        he = JS_HashTableRawAdd(table, hep, keyHash, (void *)key, NULL);
        if (!he) {
            JS_ReportOutOfMemory(cx);
            atom = NULL;
            goto out;
        }
    }

    atom = (JSAtom *)he;
    atom->flags |= flags & (ATOM_PINNED | ATOM_INTERNED | ATOM_HIDDEN);
    cx->weakRoots.lastAtom = atom;
out:
    JS_UNLOCK(&state->lock,cx);
    return atom;
}

JS_FRIEND_API(JSAtom *)
js_Atomize(JSContext *cx, const char *bytes, size_t length, uintN flags)
{
    jschar *chars;
    JSString *str;
    JSAtom *atom;
    char buf[2 * ALIGNMENT(JSString)];

    /*
     * Avoiding the malloc in js_InflateString on shorter strings saves us
     * over 20,000 malloc calls on mozilla browser startup. This compares to
     * only 131 calls where the string is longer than a 31 char (net) buffer.
     * The vast majority of atomized strings are already in the hashtable. So
     * js_AtomizeString rarely has to copy the temp string we make.
     */
#define ATOMIZE_BUF_MAX 32
    jschar inflated[ATOMIZE_BUF_MAX];
    size_t inflatedLength = ATOMIZE_BUF_MAX - 1;

    if (length < ATOMIZE_BUF_MAX) {
        js_InflateStringToBuffer(cx, bytes, length, inflated, &inflatedLength);
        inflated[inflatedLength] = 0;
        chars = inflated;
    } else {
        inflatedLength = length;
        chars = js_InflateString(cx, bytes, &inflatedLength);
        if (!chars)
            return NULL;
        flags |= ATOM_NOCOPY;
    }

    str = ALIGN(buf, JSString);

    str->chars = chars;
    str->length = inflatedLength;
    atom = js_AtomizeString(cx, str, ATOM_TMPSTR | flags);
    if (chars != inflated && (!atom || ATOM_TO_STRING(atom)->chars != chars))
        JS_free(cx, chars);
    return atom;
}

JS_FRIEND_API(JSAtom *)
js_AtomizeChars(JSContext *cx, const jschar *chars, size_t length, uintN flags)
{
    JSString *str;
    char buf[2 * ALIGNMENT(JSString)];

    str = ALIGN(buf, JSString);
    str->chars = (jschar *)chars;
    str->length = length;
    return js_AtomizeString(cx, str, ATOM_TMPSTR | flags);
}

JSAtom *
js_GetExistingStringAtom(JSContext *cx, const jschar *chars, size_t length)
{
    JSString *str;
    char buf[2 * ALIGNMENT(JSString)];
    JSHashNumber keyHash;
    jsval key;
    JSAtomState *state;
    JSHashTable *table;
    JSHashEntry **hep;

    str = ALIGN(buf, JSString);
    str->chars = (jschar *)chars;
    str->length = length;
    keyHash = js_HashString(str);
    key = STRING_TO_JSVAL(str);
    state = &cx->runtime->atomState;
    JS_LOCK(&state->lock, cx);
    table = state->table;
    hep = JS_HashTableRawLookup(table, keyHash, (void *)key);
    JS_UNLOCK(&state->lock, cx);
    return (hep) ? (JSAtom *)*hep : NULL;
}

JSAtom *
js_AtomizeValue(JSContext *cx, jsval value, uintN flags)
{
    if (JSVAL_IS_STRING(value))
        return js_AtomizeString(cx, JSVAL_TO_STRING(value), flags);
    if (JSVAL_IS_INT(value))
        return js_AtomizeInt(cx, JSVAL_TO_INT(value), flags);
    if (JSVAL_IS_DOUBLE(value))
        return js_AtomizeDouble(cx, *JSVAL_TO_DOUBLE(value), flags);
    if (JSVAL_IS_OBJECT(value))
        return js_AtomizeObject(cx, JSVAL_TO_OBJECT(value), flags);
    if (JSVAL_IS_BOOLEAN(value))
        return js_AtomizeBoolean(cx, JSVAL_TO_BOOLEAN(value), flags);
    return js_AtomizeHashedKey(cx, value, (JSHashNumber)value, flags);
}

JSAtom *
js_ValueToStringAtom(JSContext *cx, jsval v)
{
    JSString *str;

    str = js_ValueToString(cx, v);
    if (!str)
        return NULL;
    return js_AtomizeString(cx, str, 0);
}

JS_STATIC_DLL_CALLBACK(JSHashNumber)
js_hash_atom_ptr(const void *key)
{
    const JSAtom *atom = key;
    return atom->number;
}

JS_STATIC_DLL_CALLBACK(void *)
js_alloc_temp_space(void *priv, size_t size)
{
    JSContext *cx = priv;
    void *space;

    JS_ARENA_ALLOCATE(space, &cx->tempPool, size);
    if (!space)
        JS_ReportOutOfMemory(cx);
    return space;
}

JS_STATIC_DLL_CALLBACK(void)
js_free_temp_space(void *priv, void *item)
{
}

JS_STATIC_DLL_CALLBACK(JSHashEntry *)
js_alloc_temp_entry(void *priv, const void *key)
{
    JSContext *cx = priv;
    JSAtomListElement *ale;

    JS_ARENA_ALLOCATE_TYPE(ale, JSAtomListElement, &cx->tempPool);
    if (!ale) {
        JS_ReportOutOfMemory(cx);
        return NULL;
    }
    return &ale->entry;
}

JS_STATIC_DLL_CALLBACK(void)
js_free_temp_entry(void *priv, JSHashEntry *he, uintN flag)
{
}

static JSHashAllocOps temp_alloc_ops = {
    js_alloc_temp_space,    js_free_temp_space,
    js_alloc_temp_entry,    js_free_temp_entry
};

JSAtomListElement *
js_IndexAtom(JSContext *cx, JSAtom *atom, JSAtomList *al)
{
    JSAtomListElement *ale, *ale2, *next;
    JSHashEntry **hep;

    ATOM_LIST_LOOKUP(ale, hep, al, atom);
    if (!ale) {
        if (al->count < 10) {
            /* Few enough for linear search, no hash table needed. */
            JS_ASSERT(!al->table);
            ale = (JSAtomListElement *)js_alloc_temp_entry(cx, atom);
            if (!ale)
                return NULL;
            ALE_SET_ATOM(ale, atom);
            ALE_SET_NEXT(ale, al->list);
            al->list = ale;
        } else {
            /* We want to hash.  Have we already made a hash table? */
            if (!al->table) {
                /* No hash table yet, so hep had better be null! */
                JS_ASSERT(!hep);
                al->table = JS_NewHashTable(al->count + 1, js_hash_atom_ptr,
                                            JS_CompareValues, JS_CompareValues,
                                            &temp_alloc_ops, cx);
                if (!al->table)
                    return NULL;

                /*
                 * Set ht->nentries explicitly, because we are moving entries
                 * from al to ht, not calling JS_HashTable(Raw|)Add.
                 */
                al->table->nentries = al->count;

                /* Insert each ale on al->list into the new hash table. */
                for (ale2 = al->list; ale2; ale2 = next) {
                    next = ALE_NEXT(ale2);
                    ale2->entry.keyHash = ALE_ATOM(ale2)->number;
                    hep = JS_HashTableRawLookup(al->table, ale2->entry.keyHash,
                                                ale2->entry.key);
                    ALE_SET_NEXT(ale2, *hep);
                    *hep = &ale2->entry;
                }
                al->list = NULL;

                /* Set hep for insertion of atom's ale, immediately below. */
                hep = JS_HashTableRawLookup(al->table, atom->number, atom);
            }

            /* Finally, add an entry for atom into the hash bucket at hep. */
            ale = (JSAtomListElement *)
                  JS_HashTableRawAdd(al->table, hep, atom->number, atom, NULL);
            if (!ale)
                return NULL;
        }

        ALE_SET_INDEX(ale, al->count++);
    }
    return ale;
}

JS_FRIEND_API(JSAtom *)
js_GetAtom(JSContext *cx, JSAtomMap *map, jsatomid i)
{
    JSAtom *atom;
    static JSAtom dummy;

    JS_ASSERT(map->vector && i < map->length);
    if (!map->vector || i >= map->length) {
        char numBuf[12];
        JS_snprintf(numBuf, sizeof numBuf, "%lu", (unsigned long)i);
        JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL,
                             JSMSG_BAD_ATOMIC_NUMBER, numBuf);
        return &dummy;
    }
    atom = map->vector[i];
    JS_ASSERT(atom);
    return atom;
}

JS_STATIC_DLL_CALLBACK(intN)
js_map_atom(JSHashEntry *he, intN i, void *arg)
{
    JSAtomListElement *ale = (JSAtomListElement *)he;
    JSAtom **vector = arg;

    vector[ALE_INDEX(ale)] = ALE_ATOM(ale);
    return HT_ENUMERATE_NEXT;
}

#ifdef DEBUG
static jsrefcount js_atom_map_count;
static jsrefcount js_atom_map_hash_table_count;
#endif

JS_FRIEND_API(JSBool)
js_InitAtomMap(JSContext *cx, JSAtomMap *map, JSAtomList *al)
{
    JSAtom **vector;
    JSAtomListElement *ale;
    uint32 count;

#ifdef DEBUG
    JS_ATOMIC_INCREMENT(&js_atom_map_count);
#endif
    ale = al->list;
    if (!ale && !al->table) {
        map->vector = NULL;
        map->length = 0;
        return JS_TRUE;
    }

    count = al->count;
    if (count >= ATOM_INDEX_LIMIT) {
        JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL,
                             JSMSG_TOO_MANY_LITERALS);
        return JS_FALSE;
    }
    vector = (JSAtom **) JS_malloc(cx, (size_t) count * sizeof *vector);
    if (!vector)
        return JS_FALSE;

    if (al->table) {
#ifdef DEBUG
        JS_ATOMIC_INCREMENT(&js_atom_map_hash_table_count);
#endif
        JS_HashTableEnumerateEntries(al->table, js_map_atom, vector);
    } else {
        do {
            vector[ALE_INDEX(ale)] = ALE_ATOM(ale);
        } while ((ale = ALE_NEXT(ale)) != NULL);
    }
    ATOM_LIST_INIT(al);

    map->vector = vector;
    map->length = (jsatomid)count;
    return JS_TRUE;
}

JS_FRIEND_API(void)
js_FreeAtomMap(JSContext *cx, JSAtomMap *map)
{
    if (map->vector) {
        JS_free(cx, map->vector);
        map->vector = NULL;
    }
    map->length = 0;
}
