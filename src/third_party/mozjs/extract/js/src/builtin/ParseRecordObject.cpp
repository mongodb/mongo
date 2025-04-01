/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "builtin/ParseRecordObject.h"

#include "vm/JSObject-inl.h"  // NewBuiltinClassInstance

using namespace js;

// https://tc39.es/proposal-json-parse-with-source/#sec-json-parse-record

ParseRecordObject::ParseRecordObject()
    : parseNode(nullptr), key(JS::PropertyKey::Void()) {}

ParseRecordObject::ParseRecordObject(Handle<js::JSONParseNode*> parseNode,
                                     const Value& val)
    : parseNode(parseNode), key(JS::PropertyKey::Void()), value(val) {}

bool ParseRecordObject::addEntries(JSContext* cx, EntryMap&& appendEntries) {
  if (!entries) {
    entries = js::MakeUnique<EntryMap>(std::move(appendEntries));
    return !!entries;
  }
  for (auto iter = appendEntries.iter(); !iter.done(); iter.next()) {
    if (!entries->put(iter.get().key(), std::move(iter.get().value()))) {
      return false;
    }
  }
  return true;
}

void ParseRecordObject::trace(JSTracer* trc) {
  JS::TraceRoot(trc, &parseNode, "ParseRecordObject parse node");
  JS::TraceRoot(trc, &key, "ParseRecordObject key");
  JS::TraceRoot(trc, &value, "ParseRecordObject value");
  if (entries) {
    entries->trace(trc);
  }
}
