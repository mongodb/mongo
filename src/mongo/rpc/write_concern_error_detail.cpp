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

#include "mongo/rpc/write_concern_error_detail.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/error_extra_info.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/rpc/write_concern_error_gen.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

using std::string;

WriteConcernErrorDetail::WriteConcernErrorDetail() {
    clear();
}

WriteConcernErrorDetail::WriteConcernErrorDetail(Status status) {
    clear();
    setStatus(std::move(status));
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

    auto wce = WriteConcernError();
    wce.setCode(_status.code());
    wce.setCodeName(_status.codeString());
    wce.setErrmsg(_status.reason());
    wce.setErrInfo(_errInfo);
    wce.serialize(&builder);

    if (auto extra = _status.extraInfo())
        extra->serialize(&builder);

    return builder.obj();
}

bool WriteConcernErrorDetail::parseBSON(const BSONObj& source, string* errMsg) {
    clear();

    string dummy;
    if (!errMsg)
        errMsg = &dummy;

    try {
        auto wce = WriteConcernError::parse(source, IDLParserContext{"writeConcernError"});
        _status = Status(ErrorCodes::Error(wce.getCode()), wce.getErrmsg(), source);
        if ((_isErrInfoSet = wce.getErrInfo().has_value())) {
            _errInfo = wce.getErrInfo().value().getOwned();
        }
    } catch (DBException& ex) {
        *errMsg = str::stream() << ex.reason();
        return false;
    }

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

    return _status.withReason(str::stream()
                              << _status.reason() << "; Error details: " << _errInfo.toString());
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

WriteConcernErrorDetail getWriteConcernErrorDetail(const BSONElement& wcErrorElem) {
    WriteConcernErrorDetail wcError;
    std::string errMsg;
    auto wcErrorObj = wcErrorElem.Obj();
    if (!wcError.parseBSON(wcErrorObj, &errMsg)) {
        wcError.clear();
        wcError.setStatus({ErrorCodes::FailedToParse,
                           "Failed to parse writeConcernError: " + wcErrorObj.toString() +
                               ", Received error: " + errMsg});
    }

    return wcError;
}

std::unique_ptr<WriteConcernErrorDetail> getWriteConcernErrorDetailFromBSONObj(const BSONObj& obj) {
    BSONElement wcErrorElem;
    Status status = bsonExtractTypedField(obj, "writeConcernError", BSONType::object, &wcErrorElem);
    if (!status.isOK()) {
        if (status == ErrorCodes::NoSuchKey) {
            return nullptr;
        } else {
            uassertStatusOK(status);
        }
    }

    return std::make_unique<WriteConcernErrorDetail>(getWriteConcernErrorDetail(wcErrorElem));
}

}  // namespace mongo
