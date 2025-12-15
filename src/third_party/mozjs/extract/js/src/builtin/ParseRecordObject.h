/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef builtin_ParseRecordObject_h
#define builtin_ParseRecordObject_h

#include "js/HashTable.h"
#include "js/TracingAPI.h"
#include "vm/JSContext.h"

namespace js {

using JSONParseNode = JSString;

class ParseRecordObject : public NativeObject {
  enum { ParseNodeSlot, ValueSlot, KeySlot, EntriesSlot, SlotCount };

 public:
  using EntryMap = JSObject;

  static const JSClass class_;

  static ParseRecordObject* create(JSContext* cx, const Value& val);
  static ParseRecordObject* create(JSContext* cx,
                                   Handle<js::JSONParseNode*> parseNode,
                                   const Value& val);

  // The source text that was parsed for this record. According to the spec, we
  // don't track this for objects and arrays, so it will be a null pointer.
  JSONParseNode* getParseNode() const {
    const Value& slot = getSlot(ParseNodeSlot);
    return slot.isUndefined() ? nullptr : slot.toString();
  }

  // For object members, the member key. For arrays, the index. For JSON
  // primitives, it will be undefined.
  JS::PropertyKey getKey(JSContext* cx) const;

  bool setKey(JSContext* cx, const JS::PropertyKey& key);

  // The original value corresponding to this record, used to determine if the
  // reviver function has modified it.
  const Value& getValue() const { return getSlot(ValueSlot); }

  void setValue(JS::Handle<JS::Value> value) { setSlot(ValueSlot, value); }

  bool hasValue() const { return !getValue().isUndefined(); }

  // For objects and arrays, the records for the members and elements
  // (respectively). If there are none, or for JSON primitives, the entries
  // parameter is unmodified.
  void getEntries(JSContext* cx, MutableHandle<EntryMap*> entries);

  void setEntries(JSContext* cx, Handle<EntryMap*> entries);
};

}  // namespace js

#endif
