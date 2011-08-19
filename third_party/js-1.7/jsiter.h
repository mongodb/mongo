/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sw=4 et tw=78:
 *
 * ***** BEGIN LICENSE BLOCK *****
 * Version: MPL 1.1/GPL 2.0/LGPL 2.1 *
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

#ifndef jsiter_h___
#define jsiter_h___
/*
 * JavaScript iterators.
 */
#include "jsprvtd.h"
#include "jspubtd.h"

#define JSITER_ENUMERATE  0x1   /* for-in compatible hidden default iterator */
#define JSITER_FOREACH    0x2   /* return [key, value] pair rather than key */
#define JSITER_KEYVALUE   0x4   /* destructuring for-in wants [key, value] */

extern void
js_CloseNativeIterator(JSContext *cx, JSObject *iterobj);

extern void
js_CloseIteratorState(JSContext *cx, JSObject *iterobj);

/*
 * Convert the value stored in *vp to its iteration object. The flags should
 * contain JSITER_ENUMERATE if js_ValueToIterator is called when enumerating
 * for-in semantics are required, and when the caller can guarantee that the
 * iterator will never be exposed to scripts.
 */
extern JSBool
js_ValueToIterator(JSContext *cx, uintN flags, jsval *vp);

/*
 * Given iterobj, call iterobj.next().  If the iterator stopped, set *rval to
 * JSVAL_HOLE. Otherwise set it to the result of the next call.
 */
extern JSBool
js_CallIteratorNext(JSContext *cx, JSObject *iterobj, jsval *rval);

#if JS_HAS_GENERATORS

/*
 * Generator state codes.
 */
typedef enum JSGeneratorState {
    JSGEN_NEWBORN,  /* not yet started */
    JSGEN_OPEN,     /* started by a .next() or .send(undefined) call */
    JSGEN_RUNNING,  /* currently executing via .next(), etc., call */
    JSGEN_CLOSING,  /* close method is doing asynchronous return */
    JSGEN_CLOSED    /* closed, cannot be started or closed again */
} JSGeneratorState;

struct JSGenerator {
    JSGenerator         *next;
    JSObject            *obj;
    JSGeneratorState    state;
    JSStackFrame        frame;
    JSArena             arena;
    jsval               stack[1];
};

#define FRAME_TO_GENERATOR(fp) \
    ((JSGenerator *) ((uint8 *)(fp) - offsetof(JSGenerator, frame)))

extern JSObject *
js_NewGenerator(JSContext *cx, JSStackFrame *fp);

extern JSBool
js_CloseGeneratorObject(JSContext *cx, JSGenerator *gen);

#endif

extern JSClass          js_GeneratorClass;
extern JSClass          js_IteratorClass;
extern JSClass          js_StopIterationClass;

extern JSObject *
js_InitIteratorClasses(JSContext *cx, JSObject *obj);

#endif /* jsiter_h___ */
