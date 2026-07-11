// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/query/write_ops/write_ops_gen.h"
#include "mongo/util/modules.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
using namespace std::literals::string_view_literals;

inline Status validateUpdateReturn(const std::string& value) {
    if (value != "pre" && value != "post") {
        return {ErrorCodes::BadValue, "\"return\" should be either \"pre\" or \"post\""};
    }

    return Status::OK();
}

/**
 * A single item in a batch of results in a 'bulkWrite' command response.
 */
class [[MONGO_MOD_NEEDS_REPLACEMENT]] BulkWriteReplyItem {
public:
    static constexpr auto kCodeFieldName = "code"sv;
    static constexpr auto kErrmsgFieldName = "errmsg"sv;
    static constexpr auto kIdxFieldName = "idx"sv;
    static constexpr auto kNFieldName = "n"sv;
    static constexpr auto kNModifiedFieldName = "nModified"sv;
    static constexpr auto kOkFieldName = "ok"sv;
    static constexpr auto kUpsertedFieldName = "upserted"sv;

    BulkWriteReplyItem();
    BulkWriteReplyItem(std::int32_t idx, Status status = Status::OK());

    BSONObj serialize() const;
    BSONObj toBSON() const;

    /**
     * Factory function that parses a BulkWriteReplyItem from a BSONObj.
     */
    static BulkWriteReplyItem parse(const BSONObj& bsonObject);

    double getOk() const {
        return _ok;
    }

    void setOk(double value) {
        if (_cached) {
            _cached = boost::none;
        }
        validateOk(value);
        _ok = std::move(value);
        _hasOk = true;
    }

    /**
     * Holds the index of the batch entry.
     */
    std::int32_t getIdx() const {
        return _idx;
    }

    void setIdx(std::int32_t value) {
        if (_cached) {
            _cached = boost::none;
        }
        validateIdx(value);
        _idx = std::move(value);
        _hasIdx = true;
    }

    /**
     * For insert: number of documents inserted.
     * For update: number of documents that matched the query predicate.
     * For delete: number of documents deleted.
     */
    boost::optional<std::int32_t> getN() const {
        return _n;
    }

    void setN(boost::optional<std::int32_t> value) {
        if (_cached) {
            _cached = boost::none;
        }
        _n = std::move(value);
    }

    /**
     * Number of updated documents.
     */
    boost::optional<std::int32_t> getNModified() const {
        return _nModified;
    }

    void setNModified(boost::optional<std::int32_t> value) {
        if (_cached) {
            _cached = boost::none;
        }
        _nModified = std::move(value);
    }

    /**
     * Contains documents that have been upserted.
     */
    const boost::optional<IDLAnyTypeOwned>& getUpserted() const {
        return _upserted;
    }

    void setUpserted(boost::optional<IDLAnyTypeOwned> value) {
        if (_cached) {
            _cached = boost::none;
        }
        _upserted = std::move(value);
    }

    void setUpserted(boost::optional<mongo::write_ops::Upserted> value) {
        if (_cached) {
            _cached = boost::none;
        }
        if (!value) {
            _upserted = boost::none;
            return;
        }
        // BulkWrite needs only _id, not index.
        BSONObj upserted = value->toBSON().removeField("index");
        _upserted = IDLAnyTypeOwned(upserted.getField("_id"));
    }

    /**
     * The status associated with the reply potentially containing error data for why the
     * operation failed.
     */
    [[MONGO_MOD_NEEDS_REPLACEMENT]] const Status& getStatus() const {
        return _status;
    }

    void setStatus(const Status status) {
        if (_cached) {
            _cached = boost::none;
        }
        _status = std::move(status);
    }

    // Approximate size in bytes
    int32_t getApproximateSize() const {
        int32_t res = serialize().objsize();
        return res;
    }

protected:
    void parseProtected(const BSONObj& bsonObject);

private:
    void validateOk(double value);

    void validateIdx(std::int32_t value);

private:
    double _ok;
    std::int32_t _idx;
    boost::optional<std::int32_t> _n;
    boost::optional<std::int32_t> _nModified;
    boost::optional<IDLAnyTypeOwned> _upserted;
    Status _status = Status::OK();
    bool _hasOk : 1;
    bool _hasIdx : 1;
    mutable boost::optional<BSONObj> _cached;
};

}  // namespace mongo
