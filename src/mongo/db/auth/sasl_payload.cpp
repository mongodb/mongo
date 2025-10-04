/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/db/auth/sasl_payload.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/base64.h"
#include "mongo/util/str.h"

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

void SaslPayload::serializeToBSON(StringData fieldName, BSONObjBuilder* bob) const {
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
