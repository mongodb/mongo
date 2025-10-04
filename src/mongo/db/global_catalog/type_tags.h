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

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bson_field.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/namespace_string.h"

#include <string>

#include <boost/move/utility_core.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

class BSONObj;
class Status;
template <typename T>
class StatusWith;


/**
 * This class represents the layout and contents of documents contained in the config.tags
 * collection. All manipulation of documents coming from that collection should be done with
 * this class.
 */
class TagsType {
public:
    // Name of the tags collection in the config server.
    static const NamespaceString ConfigNS;

    // Field names and types in the tags collection type.
    static const BSONField<std::string> ns;
    static const BSONField<std::string> tag;
    static const BSONField<BSONObj> min;
    static const BSONField<BSONObj> max;

    TagsType() = default;
    TagsType(NamespaceString nss, std::string tag, ChunkRange range);

    /**
     * Constructs a new DatabaseType object from BSON. Validates that all required fields are
     * present.
     */
    static StatusWith<TagsType> fromBSON(const BSONObj& source);

    /**
     * Returns OK if all fields have been set. Otherwise returns NoSuchKey and information
     * about what is the first field which is missing.
     */
    Status validate() const;

    /**
     * Returns the BSON representation of the entry.
     */
    BSONObj toBSON() const;

    /**
     * Returns a std::string representation of the current internal state.
     */
    std::string toString() const;

    const NamespaceString& getNS() const {
        return _ns.get();
    }
    void setNS(const NamespaceString& ns);

    const std::string& getTag() const {
        return _tag.get();
    }
    void setTag(const std::string& tag);

    void setRange(const ChunkRange& range);
    const ChunkRange& getRange() const {
        return _range.get();
    }
    const BSONObj& getMinKey() const {
        return getRange().getMin();
    }
    const BSONObj& getMaxKey() const {
        return getRange().getMax();
    }

private:
    // Required namespace to which this tag belongs
    boost::optional<NamespaceString> _ns;

    // Required tag name
    boost::optional<std::string> _tag;

    // Required range
    boost::optional<ChunkRange> _range;
};

}  // namespace mongo
