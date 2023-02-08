/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/commands/bulk_write_parser.h"

#include <string>

#include "mongo/bson/bsonobjbuilder.h"

namespace mongo {

constexpr StringData BulkWriteReplyItem::kCodeFieldName;
constexpr StringData BulkWriteReplyItem::kErrmsgFieldName;
constexpr StringData BulkWriteReplyItem::kIdxFieldName;
constexpr StringData BulkWriteReplyItem::kNFieldName;
constexpr StringData BulkWriteReplyItem::kNModifiedFieldName;
constexpr StringData BulkWriteReplyItem::kOkFieldName;
constexpr StringData BulkWriteReplyItem::kUpsertedFieldName;
constexpr StringData BulkWriteReplyItem::kValueFieldName;


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
        throwComparisonError<double>("ok", ">="_sd, value, 0.0);
    }
    if (!(value <= 1.0)) {
        throwComparisonError<double>("ok", "<="_sd, value, 1.0);
    }
}

void BulkWriteReplyItem::validateIdx(const std::int32_t value) {
    if (!(value >= 0)) {
        throwComparisonError<std::int32_t>("idx", ">="_sd, value, 0);
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
            const auto localObject = element.Obj();
            _upserted = mongo::write_ops::Upserted::parse(ctxt, localObject);
        } else if (fieldName == kValueFieldName) {
            const BSONObj localObject = element.Obj();
            _value = BSONObj::getOwned(localObject);
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

    if (_n) {
        builder.append(kNFieldName, _n.get());
    }

    if (_nModified) {
        builder.append(kNModifiedFieldName, _nModified.get());
    }

    if (_upserted) {
        BSONObjBuilder subObjBuilder(builder.subobjStart(kUpsertedFieldName));
        _upserted.get().serialize(&subObjBuilder);
    }

    if (_value) {
        builder.append(kValueFieldName, _value.get());
    }

    return builder.obj();
}


BSONObj BulkWriteReplyItem::toBSON() const {
    return serialize();
}
}  // namespace mongo
