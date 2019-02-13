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

#include "mongo/platform/basic.h"

#include "mongo/s/write_ops/write_error_detail.h"

#include "mongo/db/field_parser.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

using std::string;

const BSONField<int> WriteErrorDetail::index("index");
const BSONField<int> WriteErrorDetail::errCode("code");
const BSONField<std::string> WriteErrorDetail::errCodeName("codeName");
const BSONField<BSONObj> WriteErrorDetail::errInfo("errInfo");
const BSONField<std::string> WriteErrorDetail::errMessage("errmsg");

WriteErrorDetail::WriteErrorDetail() {
    clear();
}

bool WriteErrorDetail::isValid(std::string* errMsg) const {
    std::string dummy;
    if (errMsg == NULL) {
        errMsg = &dummy;
    }

    // All the mandatory fields must be present.
    if (!_isIndexSet) {
        *errMsg = str::stream() << "missing " << index.name() << " field";
        return false;
    }

    // This object only makes sense when the status isn't OK
    if (_status.isOK()) {
        *errMsg = "WriteErrorDetail shouldn't have OK status.";
        return false;
    }

    return true;
}

BSONObj WriteErrorDetail::toBSON() const {
    BSONObjBuilder builder;

    if (_isIndexSet)
        builder.append(index(), _index);

    invariant(!_status.isOK());
    builder.append(errCode(), _status.code());
    builder.append(errCodeName(), _status.codeString());
    builder.append(errMessage(), _status.reason());
    if (auto extra = _status.extraInfo())
        extra->serialize(&builder);

    if (_isErrInfoSet)
        builder.append(errInfo(), _errInfo);

    return builder.obj();
}

bool WriteErrorDetail::parseBSON(const BSONObj& source, string* errMsg) {
    clear();

    std::string dummy;
    if (!errMsg)
        errMsg = &dummy;

    FieldParser::FieldState fieldState;
    fieldState = FieldParser::extract(source, index, &_index, errMsg);
    if (fieldState == FieldParser::FIELD_INVALID)
        return false;
    _isIndexSet = fieldState == FieldParser::FIELD_SET;

    int errCodeValue;
    fieldState = FieldParser::extract(source, errCode, &errCodeValue, errMsg);
    if (fieldState == FieldParser::FIELD_INVALID)
        return false;
    bool haveStatus = fieldState == FieldParser::FIELD_SET;
    std::string errMsgValue;
    fieldState = FieldParser::extract(source, errMessage, &errMsgValue, errMsg);
    if (fieldState == FieldParser::FIELD_INVALID)
        return false;
    haveStatus = haveStatus && fieldState == FieldParser::FIELD_SET;
    if (!haveStatus) {
        *errMsg = "missing code or errmsg field";
        return false;
    }
    _status = Status(ErrorCodes::Error(errCodeValue), errMsgValue, source);

    fieldState = FieldParser::extract(source, errInfo, &_errInfo, errMsg);
    if (fieldState == FieldParser::FIELD_INVALID)
        return false;
    _isErrInfoSet = fieldState == FieldParser::FIELD_SET;

    return true;
}

void WriteErrorDetail::clear() {
    _index = 0;
    _isIndexSet = false;

    _status = Status::OK();

    _errInfo = BSONObj();
    _isErrInfoSet = false;
}

void WriteErrorDetail::cloneTo(WriteErrorDetail* other) const {
    other->clear();

    other->_index = _index;
    other->_isIndexSet = _isIndexSet;

    other->_status = _status;

    other->_errInfo = _errInfo;
    other->_isErrInfoSet = _isErrInfoSet;
}

std::string WriteErrorDetail::toString() const {
    return "implement me";
}

void WriteErrorDetail::setIndex(int index) {
    _index = index;
    _isIndexSet = true;
}

bool WriteErrorDetail::isIndexSet() const {
    return _isIndexSet;
}

int WriteErrorDetail::getIndex() const {
    dassert(_isIndexSet);
    return _index;
}

Status WriteErrorDetail::toStatus() const {
    return _status;
}

void WriteErrorDetail::setErrInfo(const BSONObj& errInfo) {
    _errInfo = errInfo.getOwned();
    _isErrInfoSet = true;
}

bool WriteErrorDetail::isErrInfoSet() const {
    return _isErrInfoSet;
}

const BSONObj& WriteErrorDetail::getErrInfo() const {
    dassert(_isErrInfoSet);
    return _errInfo;
}

}  // namespace mongo
