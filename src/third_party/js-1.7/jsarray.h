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

#ifndef jsarray_h___
#define jsarray_h___
/*
 * JS Array interface.
 */
#include "jsprvtd.h"
#include "jspubtd.h"

JS_BEGIN_EXTERN_C

/* Generous sanity-bound on length (in elements) of array initialiser. */
#define ARRAY_INIT_LIMIT        JS_BIT(24)

extern JSBool
js_IdIsIndex(jsval id, jsuint *indexp);

extern JSClass js_ArrayClass;

extern JSObject *
js_InitArrayClass(JSContext *cx, JSObject *obj);

extern JSObject *
js_NewArrayObject(JSContext *cx, jsuint length, jsval *vector);

extern JSBool
js_GetLengthProperty(JSContext *cx, JSObject *obj, jsuint *lengthp);

extern JSBool
js_SetLengthProperty(JSContext *cx, JSObject *obj, jsuint length);

extern JSBool
js_HasLengthProperty(JSContext *cx, JSObject *obj, jsuint *lengthp);

/*
 * Test whether an object is "array-like".  Currently this means whether obj
 * is an Array or an arguments object.  We would like an API, and probably a
 * way in the language, to bless other objects as array-like: having indexed
 * properties, and a 'length' property of uint32 value equal to one more than
 * the greatest index.
 */
extern JSBool
js_IsArrayLike(JSContext *cx, JSObject *obj, JSBool *answerp, jsuint *lengthp);

/*
 * JS-specific heap sort function.
 */
typedef JSBool (*JSComparator)(void *arg, const void *a, const void *b,
                               int *result);

extern JSBool
js_HeapSort(void *vec, size_t nel, void *pivot, size_t elsize,
            JSComparator cmp, void *arg);

JS_END_EXTERN_C

#endif /* jsarray_h___ */
