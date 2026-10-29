// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/commands/query_cmd/bulk_write_parser.h"

#include "mongo/base/error_extra_info.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/util/assert_util.h"

#include <memory>
#include <string>
#include <string_view>

#include <boost/cstdint.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
using namespace std::literals::string_view_literals;

constexpr std::string_view BulkWriteReplyItem::kCodeFieldName;
constexpr std::string_view BulkWriteReplyItem::kErrmsgFieldName;
constexpr std::string_view BulkWriteReplyItem::kIdxFieldName;
constexpr std::string_view BulkWriteReplyItem::kNFieldName;
constexpr std::string_view BulkWriteReplyItem::kNModifiedFieldName;
constexpr std::string_view BulkWriteReplyItem::kOkFieldName;
constexpr std::string_view BulkWriteReplyItem::kUpsertedFieldName;


BulkWriteReplyItem::BulkWriteReplyItem()
    : _ok(mongo::idl::preparsedValue<decltype(_ok)>()),
      _idx(mongo::idl::preparsedValue<decltype(_idx)>()),
      _hasOk(false),
      _hasIdx(false) {
    // Used for initialization only
}
BulkWriteReplyItem::BulkWriteReplyItem(std::int32_t idx, Status status)
    : _idx(idx), _status(std::move(status)), _hasOk(true), _hasIdx(true) {
    _ok = _status.isOK() ? 1.0 : 0.0;
}

void BulkWriteReplyItem::validateOk(const double value) {
    if (!(value >= 0.0)) {
        throwComparisonError<double>("ok", ">="sv, value, 0.0);
    }
    if (!(value <= 1.0)) {
        throwComparisonError<double>("ok", "<="sv, value, 1.0);
    }
}

void BulkWriteReplyItem::validateIdx(const std::int32_t value) {
    if (!(value >= 0)) {
        throwComparisonError<std::int32_t>("idx", ">="sv, value, 0);
    }
}


BulkWriteReplyItem BulkWriteReplyItem::parse(const BSONObj& bsonObject) {
    auto object = mongo::idl::preparsedValue<BulkWriteReplyItem>();
    object.parseProtected(bsonObject);
    return object;
}

void BulkWriteReplyItem::parseProtected(const BSONObj& bsonObject) {
    boost::optional<std::int32_t> code;
    boost::optional<std::string> errmsg;
    for (const auto& element : bsonObject) {
        const auto fieldName = element.fieldNameStringData();

        if (fieldName == kOkFieldName) {
            _hasOk = true;
            auto value = element.Double();
            validateOk(value);
            _ok = std::move(value);
        } else if (fieldName == kIdxFieldName) {
            _hasIdx = true;
            auto value = element.Int();
            validateIdx(value);
            _idx = std::move(value);
        } else if (fieldName == kNFieldName) {
            _n = element.Int();
        } else if (fieldName == kNModifiedFieldName) {
            _nModified = element.Int();
        } else if (fieldName == kUpsertedFieldName) {
            IDLParserContext ctxt("bulkWrite");
            _upserted = IDLAnyTypeOwned(element.Obj().getOwned().getField("_id"));
        } else if (fieldName == kCodeFieldName) {
            code = element.Int();
        } else if (fieldName == kErrmsgFieldName) {
            errmsg = element.str();
        }
    }
    if (code) {
        uassert(ErrorCodes::BadValue, "ok must be 0.0 if error code is supplied", _ok == 0.0);
        std::string err = "";
        if (errmsg) {
            err = errmsg.get();
        }
        _status = Status(ErrorCodes::Error(code.get()), err, bsonObject);
    } else {
        uassert(ErrorCodes::BadValue, "ok must be 1.0 if no error code is supplied", _ok == 1.0);
    }
}


BSONObj BulkWriteReplyItem::serialize() const {
    if (_cached) {
        return *_cached;
    }

    invariant(_hasOk && _hasIdx);

    BSONObjBuilder builder;

    builder.append(kOkFieldName, _ok);

    builder.append(kIdxFieldName, _idx);

    if (!_status.isOK()) {
        invariant(_ok == 0.0);

        builder.append(kCodeFieldName, int32_t(_status.code()));
        builder.append(kErrmsgFieldName, _status.reason());
        if (auto extraInfo = _status.extraInfo()) {
            extraInfo->serialize(&builder);
        }
    } else {
        invariant(_ok == 1.0);
    }

    builder.append(kNFieldName, _n.value_or(0));

    if (_nModified) {
        builder.append(kNModifiedFieldName, _nModified.get());
    }

    if (_upserted) {
        BSONObjBuilder subObjBuilder(builder.subobjStart(kUpsertedFieldName));
        _upserted.get().serializeToBSON("_id", &builder);
    }

    _cached = builder.obj();
    return *_cached;
}


BSONObj BulkWriteReplyItem::toBSON() const {
    return serialize();
}
}  // namespace mongo
