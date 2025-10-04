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
enum class MONGO_MOD_PUB InternedString {
#define MONGO_MOZJS_INTERNED_STRING(name, str) name,
#include "mongo/scripting/mozjs/internedstring.defs"

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
