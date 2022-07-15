/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include <boost/optional.hpp>
#include <fmt/format.h>
#include <ostream>

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/record_id.h"
#include "mongo/db/record_id_helpers.h"
#include "mongo/db/storage/key_string.h"

namespace mongo {

/**
 * A RecordId bound for a collection scan, with an optional BSON representation for pretty printing.
 */
class RecordIdBound {
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
     * Appends a BSON respresentation of the bound to a BSONObjBuilder. If one is not explicitily
     * provided it reconstructs it from the RecordId.
     */
    void appendToBSONAs(BSONObjBuilder* builder, StringData fieldName) const {
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
