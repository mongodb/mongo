// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/matcher/matcher_type_set.h"
#include "mongo/db/matcher/schema/json_pointer.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"
#include "mongo/util/uuid.h"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mongo {

/**
 * A JSON Pointer to the key id or an array of UUIDs identifying a set of keys.
 */
class EncryptSchemaKeyId {
    friend class EncryptionInfo;
    friend class EncryptionMetadata;

public:
    enum class Type {
        kUUIDs,
        kJSONPointer,
    };

    EncryptSchemaKeyId(std::string key) : _pointer(key), _type(Type::kJSONPointer) {}

    EncryptSchemaKeyId(std::vector<UUID> keys) : _uuids(std::move(keys)), _type(Type::kUUIDs) {}

    void serializeToBSON(std::string_view fieldName, BSONObjBuilder* builder) const;

    Type type() const {
        return _type;
    }

    /**
     * Callers must check that the result of type() is kUUIDs first.
     */
    const std::vector<UUID>& uuids() const {
        tassert(9584000, "Invalid type for uuids()", _type == Type::kUUIDs);
        return _uuids;
    }

    /**
     * Callers must check that the result of type() is kJSONPointer first.
     */
    const JSONPointer& jsonPointer() const {
        tassert(9584001, "Invalid type for jsonPointer()", _type == Type::kJSONPointer);
        return _pointer;
    }

    bool operator==(const EncryptSchemaKeyId& other) const {
        if (_type != other.type()) {
            return false;
        }

        return _type == Type::kUUIDs ? _uuids == other.uuids() : _pointer == other.jsonPointer();
    }

    bool operator!=(const EncryptSchemaKeyId& other) const {
        return !(*this == other);
    }

    /**
     * IDL requires overload of all comparison operators, however for this class the only viable
     * comparison is equality.
     *
     * TODO: SERVER-39677 remove these overloads
     */
    bool operator>(const EncryptSchemaKeyId& other) const {
        MONGO_UNREACHABLE;
    }

    bool operator<(const EncryptSchemaKeyId& other) const {
        MONGO_UNREACHABLE;
    }

private:
    // The default constructor is required to exist by IDL, but is private because it does not
    // construct a valid EncryptSchemaKeyId and should not be called.
    EncryptSchemaKeyId() = default;

    JSONPointer _pointer;
    std::vector<UUID> _uuids;

    Type _type;
};

/**
 * An IDL-compatible wrapper class for MatcherTypeSet for BSON type aliases.
 * It represents a set of types or of type aliases in the match language.
 */
class BSONTypeSet {
public:
    BSONTypeSet(MatcherTypeSet typeSet) : _typeSet(std::move(typeSet)) {}

    void serializeToBSON(std::string_view fieldName, BSONObjBuilder* builder) const;

    const MatcherTypeSet& typeSet() const {
        return _typeSet;
    }

    bool operator==(const BSONTypeSet& other) const {
        return _typeSet == other._typeSet;
    }

    /**
     * IDL requires overload of all comparison operators, however for this class the only viable
     * comparison is equality. These should be removed once SERVER-39677 is implemented.
     */
    bool operator>(const BSONTypeSet& other) const {
        MONGO_UNREACHABLE;
    }

    bool operator<(const BSONTypeSet& other) const {
        MONGO_UNREACHABLE;
    }

private:
    MatcherTypeSet _typeSet;
};

}  // namespace mongo
