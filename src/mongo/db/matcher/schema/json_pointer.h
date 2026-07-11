// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/field_ref.h"
#include "mongo/util/modules.h"

#include <string>
#include <vector>

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

    FieldRef toFieldRef() const {
        FieldRef ref;
        for (auto&& part : _parsed) {
            ref.appendPart(part);
        }
        return ref;
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
