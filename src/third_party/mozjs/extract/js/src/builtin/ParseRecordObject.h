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

class ParseRecordObject {
 public:
  using EntryMap = js::GCHashMap<PropertyKey, ParseRecordObject>;

  // The source text that was parsed for this record. According to the spec, we
  // don't track this for objects and arrays, so it will be a null pointer.
  JSONParseNode* parseNode;
  // For object members, the member key. For arrays, the index. For JSON
  // primitives, it will be undefined.
  JS::PropertyKey key;
  // The original value corresponding to this record, used to determine if the
  // reviver function has modified it.
  Value value;
  // For objects and arrays, the records for the members and elements
  // (respectively). If there are none, or for JSON primitives, we won't
  // allocate an EntryMap.
  UniquePtr<EntryMap> entries;

  ParseRecordObject();
  ParseRecordObject(Handle<js::JSONParseNode*> parseNode, const Value& val);
  ParseRecordObject(ParseRecordObject&& other)
      : parseNode(std::move(other.parseNode)),
        key(std::move(other.key)),
        value(std::move(other.value)),
        entries(std::move(other.entries)) {}

  bool isEmpty() const { return value.isUndefined(); }

  bool addEntries(JSContext* cx, EntryMap&& appendEntries);

  // move assignment
  ParseRecordObject& operator=(ParseRecordObject&& other) noexcept {
    parseNode = other.parseNode;
    key = other.key;
    value = other.value;
    entries = std::move(other.entries);
    return *this;
  }

  void trace(JSTracer* trc);
};

}  // namespace js

#endif
