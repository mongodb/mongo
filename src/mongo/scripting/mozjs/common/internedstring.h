// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

#include <array>
#include <cstddef>

#include <jsapi.h>

#include <js/Id.h>
#include <js/RootingAPI.h>
#include <js/TypeDecls.h>

namespace mongo {
namespace mozjs {

/**
 * An enum that includes members for each interned string we own. These values
 * can be used with InteredStringId to get a handle to an id that matches that
 * identifier, or directly in ObjectWrapper.
 */
enum class [[MONGO_MOD_PUBLIC]] InternedString {
#define MONGO_MOZJS_INTERNED_STRING(name, str) name,
#include "mongo/scripting/mozjs/common/internedstring.defs"

#undef MONGO_MOZJS_INTERNED_STRING
    NUM_IDS,
};

/**
 * Provides a handle to an interned string id.
 */
class InternedStringId {
public:
    InternedStringId(JSContext* cx, InternedString id);

    operator JS::HandleId() {
        return _id;
    }

    operator jsid() {
        return _id;
    }

private:
    JS::RootedId _id;
};

/**
 * The scope global interned string table. This owns persistent roots to each
 * id and can lookup ids by enum identifier
 */
class InternedStringTable {
public:
    explicit InternedStringTable(JSContext* cx);
    ~InternedStringTable();

    JS::HandleId getInternedString(InternedString id) {
        return _internedStrings[static_cast<std::size_t>(id)];
    }

private:
    std::array<JS::PersistentRootedId, static_cast<std::size_t>(InternedString::NUM_IDS)>
        _internedStrings;
};

}  // namespace mozjs
}  // namespace mongo
