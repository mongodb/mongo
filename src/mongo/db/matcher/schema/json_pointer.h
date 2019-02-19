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

#include <string>
#include <vector>

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"

namespace mongo {
/**
 * A JSONPointer (RFC 6901) is a string representation for referring to an element within a JSON
 * instance. In the MongoDB context, it specifically is used to point to a particular BSON element
 * within a BSON object. This class implements JSON pointer parsing and resolution.
 *
 * It does not implement the behavior that the pointer "" returns the whole document.
 *
 * It does not implement any special behavior for the character '-' when evaluating a JSON Pointer
 * against an array.
 */
class JSONPointer {
public:
    /* Constructs a parsed representation of the JSON Pointer in 'ptr', encoded in UTF-8. Throws a
     * UserException if 'ptr' is invalid.
     *
     * This class assumes 'ptr' is valid UTF-8 and does not do any validation.
     */
    JSONPointer(std::string ptr);

    /**
     * Resolves the JSONPointer against 'source'. Returns the pointed to key-value pair in
     * BSONElement if it exists. Otherwise returns an empty (EOO) BSONElement.
     */
    BSONElement evaluate(const BSONObj& source) const;

    const std::string& toString() const {
        return _original;
    }

    bool operator==(const JSONPointer& other) const {
        return _original == other.toString();
    }


private:
    friend class EncryptSchemaKeyId;

    /**
     * Private because an empty JSONPointer is not valid, but IDL requires the default constructor.
     */
    JSONPointer() = default;

    std::vector<std::string> _parsed;

    std::string _original;
};

}  // namespace mongo
