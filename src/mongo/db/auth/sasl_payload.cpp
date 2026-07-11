// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/auth/sasl_payload.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/base64.h"
#include "mongo/util/str.h"

#include <string_view>

namespace mongo {
namespace auth {

SaslPayload SaslPayload::parseFromBSON(const BSONElement& elem) {
    if (elem.type() == BSONType::string) {
        try {
            SaslPayload ret(base64::decode(elem.valueStringDataSafe()));
            ret.serializeAsBase64(true);
            return ret;
        } catch (...) {
            auto status = exceptionToStatus();
            uasserted(status.code(),
                      str::stream() << "Failed decoding SASL payload: " << status.reason());
        }
    } else if (elem.type() == BSONType::binData) {
        uassert(ErrorCodes::BadValue,
                str::stream() << "Invalid SASLPayload subtype. Expected BinDataGeneral, got: "
                              << typeName(elem.binDataType()),
                elem.binDataType() == BinDataGeneral);
        int len = 0;
        const char* data = elem.binData(len);
        return SaslPayload(std::string(data, len));
    } else {
        uasserted(ErrorCodes::BadValue,
                  str::stream() << "Invalid SASLPayload type. Expected Base64 or BinData, got: "
                                << typeName(elem.type()));
    }
}

void SaslPayload::serializeToBSON(std::string_view fieldName, BSONObjBuilder* bob) const {
    if (_serializeAsBase64) {
        bob->append(fieldName, base64::encode(_payload));
    } else {
        bob->appendBinData(fieldName, int(_payload.size()), BinDataGeneral, _payload.c_str());
    }
}

void SaslPayload::serializeToBSON(BSONArrayBuilder* bob) const {
    if (_serializeAsBase64) {
        bob->append(base64::encode(_payload));
    } else {
        bob->appendBinData(int(_payload.size()), BinDataGeneral, _payload.c_str());
    }
}

}  // namespace auth
}  // namespace mongo
