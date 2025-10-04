/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/scripting/mozjs/internedstring.h"

#include "mongo/scripting/mozjs/implscope.h"
#include "mongo/util/assert_util.h"

#include <js/Id.h>
#include <js/RootingAPI.h>
#include <js/String.h>
#include <js/TypeDecls.h>

namespace mongo {
namespace mozjs {

InternedStringTable::InternedStringTable(JSContext* cx) {
    int i = 0;

#define MONGO_MOZJS_INTERNED_STRING(name, str)                                        \
    do {                                                                              \
        auto s = JS_AtomizeAndPinString(cx, str);                                     \
        if (!s) {                                                                     \
            uasserted(ErrorCodes::JSInterpreterFailure, "Failed to JS_InternString"); \
        }                                                                             \
        _internedStrings[i++].init(cx, JS::PropertyKey::fromPinnedString(s));         \
    } while (0);
#include "mongo/scripting/mozjs/internedstring.defs"

#undef MONGO_MOZJS_INTERNED_STRING
}

InternedStringTable::~InternedStringTable() {
    for (auto&& x : _internedStrings) {
        x.reset();
    }
}

InternedStringId::InternedStringId(JSContext* cx, InternedString id)
    : _id(cx, getScope(cx)->getInternedStringId(id)) {}

}  // namespace mozjs
}  // namespace mongo
