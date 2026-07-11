// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/error_codes.h"
#include "mongo/base/error_extra_info.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/record_id.h"
#include "mongo/util/modules.h"

#include <memory>
#include <variant>

#include <boost/optional/optional.hpp>

namespace [[MONGO_MOD_PUBLIC]] mongo {

enum class IncludeDuplicateRecordId { kOff, kOn };

/**
 * Represents an error returned from the storage engine when an attempt to insert a
 * key into a unique index fails because the same key already exists.
 */
class DuplicateKeyErrorInfo final : public ErrorExtraInfo {
public:
    using FoundValue = std::variant<std::monostate, RecordId, BSONObj>;

    static constexpr auto code = ErrorCodes::DuplicateKey;

    static std::shared_ptr<const ErrorExtraInfo> parse(const BSONObj&);

    explicit DuplicateKeyErrorInfo(const BSONObj& keyPattern,
                                   const BSONObj& keyValue,
                                   const BSONObj& collation,
                                   FoundValue&& foundValue,
                                   boost::optional<RecordId> duplicateRid);

    void serialize(BSONObjBuilder* bob) const override;

    BSONObj toBSON() const {
        BSONObjBuilder bob;
        serialize(&bob);
        return bob.obj();
    }

    const BSONObj& getKeyPattern() const {
        return _keyPattern;
    }

    const BSONObj& getDuplicatedKeyValue() const {
        return _keyValue;
    }

    const FoundValue& getFoundValue() const {
        return _foundValue;
    }

    boost::optional<RecordId> getDuplicateRid() const {
        return _duplicateRid;
    }

    const BSONObj& getCollation() const {
        return _collation;
    }

private:
    BSONObj _keyPattern;
    BSONObj _keyValue;

    // An empty object if the index which resulted in the duplicate key error has the simple
    // collation, otherwise gives the index's collation.
    BSONObj _collation;

    // Optionally, the value found at the cursor which produced the DuplicateKey error, for
    // diagnostic use. If the error came from an _id index, then the value will be the record id of
    // the duplicate document. If the error came from a clustered collection, then the value will be
    // the duplicate document itself.
    FoundValue _foundValue;

    // Optionally, the record id found at the cursor which produced the DuplicateKey error.
    boost::optional<RecordId> _duplicateRid;
};

}  // namespace mongo
