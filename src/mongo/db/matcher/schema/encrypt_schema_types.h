/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include <string>
#include <vector>

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/util/uuid.h"

namespace mongo {

/**
 * A string pointing to the key id or an array of UUIDs identifying a set of keys.
 */
class EncryptSchemaKeyId {
    friend class EncryptionInfoNormalized;
    friend class EncryptionMetadata;
    friend class EncryptionInfo;

public:
    enum class Type {
        kUUIDs,
        kJSONPointer,
    };

    static EncryptSchemaKeyId parseFromBSON(const BSONElement& element);

    EncryptSchemaKeyId(const std::string key)
        : _strKeyId(std::move(key)), _type(Type::kJSONPointer) {}

    EncryptSchemaKeyId(std::vector<UUID> keys) : _uuids(std::move(keys)), _type(Type::kUUIDs) {}

    void serializeToBSON(StringData fieldName, BSONObjBuilder* builder) const;

    Type type() const {
        return _type;
    }

    /**
     * Callers must check that the result of type() is kUUIDs first.
     */
    const std::vector<UUID>& uuids() const {
        invariant(_type == Type::kUUIDs);
        return _uuids;
    }

    /**
     * Callers must check that the result of type() is kJSONPointer first.
     */
    const std::string& jsonPointer() const {
        invariant(_type == Type::kJSONPointer);
        return _strKeyId;
    }

private:
    // The default constructor is required to exist by IDL, but is private because it does not
    // construct a valid EncryptSchemaKeyId and should not be called.
    EncryptSchemaKeyId() = default;

    std::string _strKeyId;
    std::vector<UUID> _uuids;

    Type _type;
};

/**
 * Class to represent an element with any type from IDL. The caller must ensure that the backing
 * BSON stays alive while this type is in use.
 */
class EncryptSchemaAnyType {
public:
    /**
     * This type is currenty only used for serialization, not parsing.
     */
    static EncryptSchemaAnyType parseFromBSON(const BSONElement& element) {
        MONGO_UNREACHABLE;
    }

    void serializeToBSON(StringData fieldName, BSONObjBuilder* builder) const {
        builder->appendAs(_element, fieldName);
    }

    void setElement(const BSONElement& element) {
        _element = element;
    }

private:
    BSONElement _element;
};

}  // namespace mongo
