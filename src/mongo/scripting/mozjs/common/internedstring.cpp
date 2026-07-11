// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/scripting/mozjs/common/internedstring.h"

#include "mongo/scripting/mozjs/common/runtime.h"
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
#include "mongo/scripting/mozjs/common/internedstring.defs"

#undef MONGO_MOZJS_INTERNED_STRING
}

InternedStringTable::~InternedStringTable() {
    for (auto&& x : _internedStrings) {
        x.reset();
    }
}

InternedStringId::InternedStringId(JSContext* cx, InternedString id)
    : _id(cx, getCommonRuntime(cx)->getInternedStringId(id)) {}

}  // namespace mozjs
}  // namespace mongo
