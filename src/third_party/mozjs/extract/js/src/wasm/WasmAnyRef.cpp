/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 *
 * Copyright 2023 Mozilla Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "wasm/WasmAnyRef.h"

#include "vm/JSContext.h"
#include "vm/JSObject.h"
#include "vm/NativeObject.h"

#include "vm/JSObject-inl.h"

using namespace js;
using namespace js::wasm;

class WasmValueBox : public NativeObject {
 public:
  static const unsigned VALUE_SLOT = 0;
  static const unsigned RESERVED_SLOTS = 1;
  static const JSClass class_;

  static WasmValueBox* create(JSContext* cx, HandleValue value);
  Value value() const { return getFixedSlot(VALUE_SLOT); }
};

const JSClass WasmValueBox::class_ = {
    "WasmValueBox", JSCLASS_HAS_RESERVED_SLOTS(RESERVED_SLOTS)};

WasmValueBox* WasmValueBox::create(JSContext* cx, HandleValue value) {
  WasmValueBox* obj = NewObjectWithGivenProto<WasmValueBox>(cx, nullptr);
  if (!obj) {
    return nullptr;
  }
  obj->setFixedSlot(VALUE_SLOT, value);
  return obj;
}

const JSClass* AnyRef::valueBoxClass() { return &WasmValueBox::class_; }

size_t AnyRef::valueBoxOffsetOfValue() {
  return NativeObject::getFixedSlotOffset(WasmValueBox::VALUE_SLOT);
}

bool AnyRef::fromJSValue(JSContext* cx, HandleValue value,
                         MutableHandleAnyRef result) {
  if (value.isNull()) {
    result.set(AnyRef::null());
    return true;
  }

  if (value.isString()) {
    JSString* string = value.toString();
    result.set(AnyRef::fromJSString(string));
    return true;
  }

  if (value.isObject()) {
    JSObject& obj = value.toObject();
    MOZ_ASSERT(!obj.is<WasmValueBox>());
    MOZ_ASSERT(obj.compartment() == cx->compartment());
    result.set(AnyRef::fromJSObject(obj));
    return true;
  }

  if (value.isInt32() && !int32NeedsBoxing(value.toInt32())) {
    result.set(AnyRef::fromInt32(value.toInt32()));
    return true;
  }

  if (value.isDouble()) {
    double doubleValue = value.toDouble();
    int32_t intValue;
    if (mozilla::NumberIsInt32(doubleValue, &intValue) &&
        !int32NeedsBoxing(intValue)) {
      result.set(AnyRef::fromInt32(intValue));
      return true;
    }
  }

  JSObject* box = AnyRef::boxValue(cx, value);
  if (!box) {
    return false;
  }
  result.set(AnyRef::fromJSObject(*box));
  return true;
}

JSObject* AnyRef::boxValue(JSContext* cx, HandleValue value) {
  MOZ_ASSERT(AnyRef::valueNeedsBoxing(value));
  return WasmValueBox::create(cx, value);
}

Value wasm::AnyRef::toJSValue() const {
  // If toJSValue needs to allocate then we need a more complicated API, and
  // we need to root the value in the callers, see comments in callExport().
  Value value;
  if (isNull()) {
    value.setNull();
  } else if (isJSString()) {
    value.setString(toJSString());
  } else if (isI31()) {
    value.setInt32(toI31());
  } else {
    JSObject& obj = toJSObject();
    if (obj.is<WasmValueBox>()) {
      value = obj.as<WasmValueBox>().value();
    } else {
      value.setObject(obj);
    }
  }
  return value;
}
