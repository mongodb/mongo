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
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/util/modules.h"

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
    void serializeToBSON(StringData fieldName, BSONObjBuilder* builder) const;

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
