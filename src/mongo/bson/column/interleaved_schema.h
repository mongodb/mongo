/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"

#include <cstdint>
#include <vector>

namespace mongo {

/**
 * Represents the fixed field layout of an interleaved BSONColumn section, discovered from the
 * reference object. The reference object defines the field names, nesting, and types for all
 * elements in the section. We discover this structure once and decode all subsequent elements
 * against it.
 *
 * Caller must ensure the reference BSONObj outlives this schema — Entry::fieldName points into it.
 */
class InterleavedSchema {
public:
    enum class Op : uint8_t {
        kEnterSubObj,  // Write subobj header (type + fieldName + null + 4-byte size)
        kScalar,       // Decode next scalar via DecodingState
        kExitSubObj,   // Write EOO + fill in size
    };

    struct Entry {
        Op op;
        StringData fieldName;  // Points into referenceObj
        uint16_t stateIdx;     // kScalar: index into decoder states
        BSONType type;         // kEnterSubObj/kExitSubObj: sub-object type
        bool allowEmpty;       // kEnterSubObj/kExitSubObj: whether empty subobj is valid
    };

    /**
     * Discovers the schema by walking the reference object. 'arrays' controls whether array
     * fields are treated as sub-objects (true) or scalars (false).
     */
    InterleavedSchema(const BSONObj& referenceObj, BSONType rootType, bool arrays);

    const std::vector<Entry>& entries() const {
        return _entries;
    }

    uint16_t scalarCount() const {
        return _scalarCount;
    }

private:
    void _discover(const BSONObj& obj,
                   StringData fieldName,
                   BSONType type,
                   bool allowEmpty,
                   uint16_t& scalarIdx);

    std::vector<Entry> _entries;
    uint16_t _scalarCount = 0;
    bool _arrays;
};

}  // namespace mongo
