// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bson_field.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/namespace_string.h"
#include "mongo/util/modules.h"

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
class [[MONGO_MOD_NEEDS_REPLACEMENT]] TagsType {
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
