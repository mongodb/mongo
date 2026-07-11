// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/rpc/write_concern_error_detail.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/error_extra_info.h"
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
