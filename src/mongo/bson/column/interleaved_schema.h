// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"

#include <cstdint>
#include <string_view>
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
    using index_t = uint32_t;

    enum class Op : uint8_t {
        kEnterSubObj,  // Write subobj header (type + fieldName + null + 4-byte size)
        kScalar,       // Decode next scalar via DecodingState
        kExitSubObj,   // Write EOO + fill in size
    };

    struct Entry {
        Op op;
        std::string_view fieldName;  // Points into referenceObj
        index_t stateIdx;            // kScalar: index into decoder states
        BSONType type;               // kEnterSubObj/kExitSubObj: sub-object type
        bool allowEmpty;             // kEnterSubObj/kExitSubObj: whether empty subobj is valid
    };

    /**
     * Discovers the schema by walking the reference object. 'arrays' controls whether array
     * fields are treated as sub-objects (true) or scalars (false).
     */
    InterleavedSchema(const BSONObj& referenceObj, BSONType rootType, bool arrays);

    const std::vector<Entry>& entries() const {
        return _entries;
    }

    index_t scalarCount() const {
        return _scalarCount;
    }

private:
    void _discover(const BSONObj& obj,
                   std::string_view fieldName,
                   BSONType type,
                   bool allowEmpty,
                   index_t& scalarIdx);

    std::vector<Entry> _entries;
    index_t _scalarCount = 0;
    bool _arrays;
};

}  // namespace mongo
