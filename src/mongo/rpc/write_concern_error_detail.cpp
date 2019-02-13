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

#include "mongo/rpc/write_concern_error_detail.h"

#include "mongo/db/field_parser.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

using std::string;

namespace {

const BSONField<int> errCode("code");
const BSONField<string> errCodeName("codeName");
const BSONField<BSONObj> errInfo("errInfo");
const BSONField<string> errMessage("errmsg");

}  // namespace

WriteConcernErrorDetail::WriteConcernErrorDetail() {
    clear();
}

bool WriteConcernErrorDetail::isValid(string* errMsg) const {
    // This object only makes sense when the status isn't OK
    if (_status.isOK()) {
        if (errMsg) {
            *errMsg = "WriteConcernError shouldn't have OK status.";
        }
        return false;
    }

    return true;
}

BSONObj WriteConcernErrorDetail::toBSON() const {
    BSONObjBuilder builder;

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

bool WriteConcernErrorDetail::parseBSON(const BSONObj& source, string* errMsg) {
    clear();

    string dummy;
    if (!errMsg)
        errMsg = &dummy;

    FieldParser::FieldState fieldState;
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

void WriteConcernErrorDetail::clear() {
    _status = Status::OK();

    _errInfo = BSONObj();
    _isErrInfoSet = false;
}

void WriteConcernErrorDetail::cloneTo(WriteConcernErrorDetail* other) const {
    other->clear();

    other->_status = _status;

    other->_errInfo = _errInfo;
    other->_isErrInfoSet = _isErrInfoSet;
}

string WriteConcernErrorDetail::toString() const {
    return str::stream() << _status.toString()
                         << "; Error details: " << (_isErrInfoSet ? _errInfo.toString() : "<none>");
}

Status WriteConcernErrorDetail::toStatus() const {
    if (!_isErrInfoSet) {
        return _status;
    }

    return _status.withReason(
        str::stream() << _status.reason() << "; Error details: " << _errInfo.toString());
}

void WriteConcernErrorDetail::setErrInfo(const BSONObj& errInfo) {
    _errInfo = errInfo.getOwned();
    _isErrInfoSet = true;
}

bool WriteConcernErrorDetail::isErrInfoSet() const {
    return _isErrInfoSet;
}

const BSONObj& WriteConcernErrorDetail::getErrInfo() const {
    dassert(_isErrInfoSet);
    return _errInfo;
}

}  // namespace mongo
