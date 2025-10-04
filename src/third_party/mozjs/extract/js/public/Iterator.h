/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef js_Iterator_h
#define js_Iterator_h

#include "js/TypeDecls.h"

namespace JS {

// https://tc39.es/ecma262/#sec-getiterator
// GetIterator
JSObject* GetIteratorObject(JSContext* cx, Handle<Value> obj, bool isAsync);

// https://tc39.es/ecma262/#sec-iteratornext
bool IteratorNext(JSContext* cx, Handle<JSObject*> iteratorRecord,
                  MutableHandle<Value> result);

// https://tc39.es/ecma262/#sec-iteratorcomplete
bool IteratorComplete(JSContext* cx, Handle<JSObject*> iterResult, bool* done);

// https://tc39.es/ecma262/#sec-iteratorvalue
bool IteratorValue(JSContext* cx, Handle<JSObject*> iterResult,
                   MutableHandle<Value> value);

// Implements iteratorRecord.[[Iterator]]
bool GetIteratorRecordIterator(JSContext* cx, Handle<JSObject*> iteratorRecord,
                               MutableHandle<Value> iterator);

// Implements GetMethod(iterator, "return").
bool GetReturnMethod(JSContext* cx, Handle<Value> iterator,
                     MutableHandle<Value> result);

}  // namespace JS

#endif /* js_Iterator_h */
