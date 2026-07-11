// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/record_id.h"
#include "mongo/db/record_id_helpers.h"
#include "mongo/util/modules.h"

#include <compare>
#include <ostream>
#include <string_view>

#include <boost/optional.hpp>

namespace mongo {

/**
 * A RecordId bound for a collection scan, with an optional BSON representation for pretty printing.
 */
class [[MONGO_MOD_PUBLIC]] RecordIdBound {
public:
    RecordIdBound() = default;

    explicit RecordIdBound(RecordId&& recordId, boost::optional<BSONObj> bson = boost::none)
        : _recordId(std::move(recordId)), _bson(bson) {}

    explicit RecordIdBound(const RecordId& recordId, boost::optional<BSONObj> bson = boost::none)
        : _recordId(recordId), _bson(bson) {}

    const RecordId& recordId() const {
        return _recordId;
    }

    /**
     * Appends a BSON respresentation of the bound to a BSONObjBuilder. If one is not explicitly
     * provided it reconstructs it from the RecordId.
     */
    void appendToBSONAs(BSONObjBuilder* builder, std::string_view fieldName) const {
        if (_bson) {
            builder->appendAs(_bson->firstElement(), fieldName);
        } else {
            record_id_helpers::appendToBSONAs(_recordId, builder, fieldName);
        }
    }

    std::string toString() const {
        return _recordId.toString();
    }

    /**
     * Compares the underlying RecordIds.
     */
    int compare(const RecordIdBound& rhs) const {
        return _recordId.compare(rhs._recordId);
    }

    std::strong_ordering operator<=>(const RecordIdBound& rhs) const {
        return compare(rhs) <=> 0;
    }

    bool operator==(const RecordIdBound& rhs) const {
        return std::is_eq(*this <=> rhs);
    }
    bool operator!=(const RecordIdBound& rhs) const = default;

private:
    RecordId _recordId;
    boost::optional<BSONObj> _bson;
};

inline StringBuilder& operator<<(StringBuilder& stream, const RecordIdBound& id) {
    return stream << "RecordIdBound(" << id.toString() << ')';
}

inline std::ostream& operator<<(std::ostream& stream, const RecordIdBound& id) {
    return stream << "RecordIdBound(" << id.toString() << ')';
}

}  // namespace mongo
