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
 * JS boolean implementation.
 */
#include "jsstddef.h"
#include "jstypes.h"
#include "jsutil.h" /* Added by JSIFY */
#include "jsapi.h"
#include "jsatom.h"
#include "jsbool.h"
#include "jscntxt.h"
#include "jsconfig.h"
#include "jsinterp.h"
#include "jslock.h"
#include "jsnum.h"
#include "jsobj.h"
#include "jsstr.h"

JSClass js_BooleanClass = {
    "Boolean",
    JSCLASS_HAS_PRIVATE | JSCLASS_HAS_CACHED_PROTO(JSProto_Boolean),
    JS_PropertyStub,  JS_PropertyStub,  JS_PropertyStub,  JS_PropertyStub,
    JS_EnumerateStub, JS_ResolveStub,   JS_ConvertStub,   JS_FinalizeStub,
    JSCLASS_NO_OPTIONAL_MEMBERS
};

#if JS_HAS_TOSOURCE
#include "jsprf.h"

static JSBool
bool_toSource(JSContext *cx, JSObject *obj, uintN argc, jsval *argv,
              jsval *rval)
{
    jsval v;
    char buf[32];
    JSString *str;

    if (JSVAL_IS_BOOLEAN((jsval)obj)) {
        v = (jsval)obj;
    } else {
        if (!JS_InstanceOf(cx, obj, &js_BooleanClass, argv))
            return JS_FALSE;
        v = OBJ_GET_SLOT(cx, obj, JSSLOT_PRIVATE);
        if (!JSVAL_IS_BOOLEAN(v))
            return js_obj_toSource(cx, obj, argc, argv, rval);
    }
    JS_snprintf(buf, sizeof buf, "(new %s(%s))",
                js_BooleanClass.name,
                js_boolean_strs[JSVAL_TO_BOOLEAN(v) ? 1 : 0]);
    str = JS_NewStringCopyZ(cx, buf);
    if (!str)
        return JS_FALSE;
    *rval = STRING_TO_JSVAL(str);
    return JS_TRUE;
}
#endif

static JSBool
bool_toString(JSContext *cx, JSObject *obj, uintN argc, jsval *argv,
              jsval *rval)
{
    jsval v;
    JSAtom *atom;
    JSString *str;

    if (JSVAL_IS_BOOLEAN((jsval)obj)) {
        v = (jsval)obj;
    } else {
        if (!JS_InstanceOf(cx, obj, &js_BooleanClass, argv))
            return JS_FALSE;
        v = OBJ_GET_SLOT(cx, obj, JSSLOT_PRIVATE);
        if (!JSVAL_IS_BOOLEAN(v))
            return js_obj_toString(cx, obj, argc, argv, rval);
    }
    atom = cx->runtime->atomState.booleanAtoms[JSVAL_TO_BOOLEAN(v) ? 1 : 0];
    str = ATOM_TO_STRING(atom);
    if (!str)
        return JS_FALSE;
    *rval = STRING_TO_JSVAL(str);
    return JS_TRUE;
}

static JSBool
bool_valueOf(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval)
{
    if (JSVAL_IS_BOOLEAN((jsval)obj)) {
        *rval = (jsval)obj;
        return JS_TRUE;
    }
    if (!JS_InstanceOf(cx, obj, &js_BooleanClass, argv))
        return JS_FALSE;
    *rval = OBJ_GET_SLOT(cx, obj, JSSLOT_PRIVATE);
    return JS_TRUE;
}

static JSFunctionSpec boolean_methods[] = {
#if JS_HAS_TOSOURCE
    {js_toSource_str,   bool_toSource,          0,JSFUN_THISP_BOOLEAN,0},
#endif
    {js_toString_str,   bool_toString,          0,JSFUN_THISP_BOOLEAN,0},
    {js_valueOf_str,    bool_valueOf,           0,JSFUN_THISP_BOOLEAN,0},
    {0,0,0,0,0}
};

static JSBool
Boolean(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval)
{
    JSBool b;
    jsval bval;

    if (argc != 0) {
        if (!js_ValueToBoolean(cx, argv[0], &b))
            return JS_FALSE;
        bval = BOOLEAN_TO_JSVAL(b);
    } else {
        bval = JSVAL_FALSE;
    }
    if (!(cx->fp->flags & JSFRAME_CONSTRUCTING)) {
        *rval = bval;
        return JS_TRUE;
    }
    OBJ_SET_SLOT(cx, obj, JSSLOT_PRIVATE, bval);
    return JS_TRUE;
}

JSObject *
js_InitBooleanClass(JSContext *cx, JSObject *obj)
{
    JSObject *proto;

    proto = JS_InitClass(cx, obj, NULL, &js_BooleanClass, Boolean, 1,
                        NULL, boolean_methods, NULL, NULL);
    if (!proto)
        return NULL;
    OBJ_SET_SLOT(cx, proto, JSSLOT_PRIVATE, JSVAL_FALSE);
    return proto;
}

JSObject *
js_BooleanToObject(JSContext *cx, JSBool b)
{
    JSObject *obj;

    obj = js_NewObject(cx, &js_BooleanClass, NULL, NULL);
    if (!obj)
        return NULL;
    OBJ_SET_SLOT(cx, obj, JSSLOT_PRIVATE, BOOLEAN_TO_JSVAL(b));
    return obj;
}

JSString *
js_BooleanToString(JSContext *cx, JSBool b)
{
    return ATOM_TO_STRING(cx->runtime->atomState.booleanAtoms[b ? 1 : 0]);
}

JSBool
js_ValueToBoolean(JSContext *cx, jsval v, JSBool *bp)
{
    JSBool b;
    jsdouble d;

    if (JSVAL_IS_NULL(v) || JSVAL_IS_VOID(v)) {
        b = JS_FALSE;
    } else if (JSVAL_IS_OBJECT(v)) {
        if (!JS_VERSION_IS_ECMA(cx)) {
            if (!OBJ_DEFAULT_VALUE(cx, JSVAL_TO_OBJECT(v), JSTYPE_BOOLEAN, &v))
                return JS_FALSE;
            if (!JSVAL_IS_BOOLEAN(v))
                v = JSVAL_TRUE;         /* non-null object is true */
            b = JSVAL_TO_BOOLEAN(v);
        } else {
            b = JS_TRUE;
        }
    } else if (JSVAL_IS_STRING(v)) {
        b = JSSTRING_LENGTH(JSVAL_TO_STRING(v)) ? JS_TRUE : JS_FALSE;
    } else if (JSVAL_IS_INT(v)) {
        b = JSVAL_TO_INT(v) ? JS_TRUE : JS_FALSE;
    } else if (JSVAL_IS_DOUBLE(v)) {
        d = *JSVAL_TO_DOUBLE(v);
        b = (!JSDOUBLE_IS_NaN(d) && d != 0) ? JS_TRUE : JS_FALSE;
    } else {
        JS_ASSERT(JSVAL_IS_BOOLEAN(v));
        b = JSVAL_TO_BOOLEAN(v);
    }

    *bp = b;
    return JS_TRUE;
}
