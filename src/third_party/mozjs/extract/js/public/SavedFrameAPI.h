/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * Functions and types related to SavedFrame objects created by the Debugger
 * API.
 */

#ifndef js_SavedFrameAPI_h
#define js_SavedFrameAPI_h

#include "jstypes.h"  // JS_PUBLIC_API

#include "js/ColumnNumber.h"  // JS::TaggedColumnNumberOneOrigin
#include "js/TypeDecls.h"

struct JSPrincipals;

namespace JS {

/*
 * Accessors for working with SavedFrame JSObjects
 *
 * Each of these functions assert that if their `HandleObject savedFrame`
 * argument is non-null, its JSClass is the SavedFrame class (or it is a
 * cross-compartment or Xray wrapper around an object with the SavedFrame class)
 * and the object is not the SavedFrame.prototype object.
 *
 * Each of these functions will find the first SavedFrame object in the chain
 * whose underlying stack frame principals are subsumed by the given
 * |principals|, and operate on that SavedFrame object. This prevents leaking
 * information about privileged frames to un-privileged callers
 *
 * The SavedFrame in parameters do _NOT_ need to be in the same compartment as
 * the cx, and the various out parameters are _NOT_ guaranteed to be in the same
 * compartment as cx.
 *
 * You may consider or skip over self-hosted frames by passing
 * `SavedFrameSelfHosted::Include` or `SavedFrameSelfHosted::Exclude`
 * respectively.
 *
 * Additionally, it may be the case that there is no such SavedFrame object
 * whose captured frame's principals are subsumed by |principals|! If the
 * `HandleObject savedFrame` argument is null, or the |principals| do not
 * subsume any of the chained SavedFrame object's principals,
 * `SavedFrameResult::AccessDenied` is returned and a (hopefully) sane default
 * value is chosen for the out param.
 *
 * See also `js/src/doc/SavedFrame/SavedFrame.md`.
 */

enum class SavedFrameResult { Ok, AccessDenied };

enum class SavedFrameSelfHosted { Include, Exclude };

/**
 * Given a SavedFrame JSObject, get its source property. Defaults to the empty
 * string.
 */
extern JS_PUBLIC_API SavedFrameResult GetSavedFrameSource(
    JSContext* cx, JSPrincipals* principals, Handle<JSObject*> savedFrame,
    MutableHandle<JSString*> sourcep,
    SavedFrameSelfHosted selfHosted = SavedFrameSelfHosted::Include);

/**
 * Given a SavedFrame JSObject, get an ID identifying its ScriptSource.
 * Defaults to 0.
 */
extern JS_PUBLIC_API SavedFrameResult GetSavedFrameSourceId(
    JSContext* cx, JSPrincipals* principals, Handle<JSObject*> savedFrame,
    uint32_t* sourceIdp,
    SavedFrameSelfHosted selfHosted = SavedFrameSelfHosted::Include);

/**
 * Given a SavedFrame JSObject, get its line property (1-origin).
 * Defaults to 0.
 */
extern JS_PUBLIC_API SavedFrameResult GetSavedFrameLine(
    JSContext* cx, JSPrincipals* principals, Handle<JSObject*> savedFrame,
    uint32_t* linep,
    SavedFrameSelfHosted selfHosted = SavedFrameSelfHosted::Include);

/**
 * Given a SavedFrame JSObject, get its column property. Defaults to 0.
 */
extern JS_PUBLIC_API SavedFrameResult GetSavedFrameColumn(
    JSContext* cx, JSPrincipals* principals, Handle<JSObject*> savedFrame,
    JS::TaggedColumnNumberOneOrigin* columnp,
    SavedFrameSelfHosted selfHosted = SavedFrameSelfHosted::Include);

/**
 * Given a SavedFrame JSObject, get its functionDisplayName string, or nullptr
 * if SpiderMonkey was unable to infer a name for the captured frame's
 * function. Defaults to nullptr.
 */
extern JS_PUBLIC_API SavedFrameResult GetSavedFrameFunctionDisplayName(
    JSContext* cx, JSPrincipals* principals, Handle<JSObject*> savedFrame,
    MutableHandle<JSString*> namep,
    SavedFrameSelfHosted selfHosted = SavedFrameSelfHosted::Include);

/**
 * Given a SavedFrame JSObject, get its asyncCause string. Defaults to nullptr.
 */
extern JS_PUBLIC_API SavedFrameResult GetSavedFrameAsyncCause(
    JSContext* cx, JSPrincipals* principals, Handle<JSObject*> savedFrame,
    MutableHandle<JSString*> asyncCausep,
    SavedFrameSelfHosted selfHosted = SavedFrameSelfHosted::Include);

/**
 * Given a SavedFrame JSObject, get its asyncParent SavedFrame object or nullptr
 * if there is no asyncParent. The `asyncParentp` out parameter is _NOT_
 * guaranteed to be in the cx's compartment. Defaults to nullptr.
 */
extern JS_PUBLIC_API SavedFrameResult GetSavedFrameAsyncParent(
    JSContext* cx, JSPrincipals* principals, Handle<JSObject*> savedFrame,
    MutableHandle<JSObject*> asyncParentp,
    SavedFrameSelfHosted selfHosted = SavedFrameSelfHosted::Include);

/**
 * Given a SavedFrame JSObject, get its parent SavedFrame object or nullptr if
 * it is the oldest frame in the stack. The `parentp` out parameter is _NOT_
 * guaranteed to be in the cx's compartment. Defaults to nullptr.
 */
extern JS_PUBLIC_API SavedFrameResult GetSavedFrameParent(
    JSContext* cx, JSPrincipals* principals, Handle<JSObject*> savedFrame,
    MutableHandle<JSObject*> parentp,
    SavedFrameSelfHosted selfHosted = SavedFrameSelfHosted::Include);

/**
 * Given a SavedFrame object, convert it and its transitive parents to plain
 * objects. Because SavedFrame objects store their properties on the prototype,
 * they cannot be usefully stringified to JSON. Assigning their properties to
 * plain objects allow those objects to be stringified and the saved frame stack
 * can be encoded as a string.
 */
JS_PUBLIC_API JSObject* ConvertSavedFrameToPlainObject(
    JSContext* cx, JS::HandleObject savedFrame,
    JS::SavedFrameSelfHosted selfHosted);

}  // namespace JS

namespace js {

/**
 * Get the first SavedFrame object in this SavedFrame stack whose principals are
 * subsumed by the given |principals|. If there is no such frame, return
 * nullptr.
 *
 * Do NOT pass a non-SavedFrame object here.
 */
extern JS_PUBLIC_API JSObject* GetFirstSubsumedSavedFrame(
    JSContext* cx, JSPrincipals* principals, JS::Handle<JSObject*> savedFrame,
    JS::SavedFrameSelfHosted selfHosted);

}  // namespace js

#endif /* js_SavedFrameAPI_h */
