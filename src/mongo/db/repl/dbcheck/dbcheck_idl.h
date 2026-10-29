// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/util/modules.h"

#include <string_view>

// TODO SERVER-78399 Remove BSONKey class once featureFlagSecondaryIndexChecksInDbCheck is removed.
namespace mongo {

/**
 * Delimits ranges of _ids for dbCheck.
 *
 * Allows wrapping and comparing arbitrary BSONElements.
 */
class BSONKey {
public:
    /**
     * Generate a BSONKey from the provided BSONElement.
     *
     * Discards `element`'s field name.
     */
    static BSONKey parseFromBSON(const BSONElement& element);

    /**
     * A BSONKey that compares less than any other BSONKey.
     */
    static BSONKey min(void);

    /**
     * A BSONKey that compares greater than any other BSONKey.
     */
    static BSONKey max(void);

    BSONKey(void) {}

    /**
     * Serialize this class as a field in a document.
     */
    void serializeToBSON(std::string_view fieldName, BSONObjBuilder* builder) const;

    const BSONObj& obj() const;
    BSONElement elem() const;

    /**
     * Lexicographical comparison between BSONKeys.
     */
    bool operator==(const BSONKey& other) const;
    bool operator!=(const BSONKey& other) const;
    bool operator<(const BSONKey& other) const;
    bool operator<=(const BSONKey& other) const;
    bool operator>(const BSONKey& other) const;
    bool operator>=(const BSONKey& other) const;

    /**
     * Lexicographical comparison between BSONKeys and BSONElements, ignoring field names.
     */
    bool operator==(const BSONElement& other) const;
    bool operator!=(const BSONElement& other) const;
    bool operator<(const BSONElement& other) const;
    bool operator<=(const BSONElement& other) const;
    bool operator>(const BSONElement& other) const;
    bool operator>=(const BSONElement& other) const;

private:
    explicit BSONKey(const BSONElement& elem);
    BSONObj _obj;
};
}  // namespace mongo
