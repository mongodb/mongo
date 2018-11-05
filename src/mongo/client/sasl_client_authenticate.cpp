
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

#include "mongo/client/sasl_client_authenticate.h"

#include <string>

#include "mongo/base/string_data.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/auth/sasl_command_constants.h"
#include "mongo/util/base64.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

using namespace mongoutils;

Future<void> (*saslClientAuthenticate)(auth::RunCommandHook runCommand,
                                       const HostAndPort& hostname,
                                       const BSONObj& saslParameters) = nullptr;

Status saslExtractPayload(const BSONObj& cmdObj, std::string* payload, BSONType* type) {
    BSONElement payloadElement;
    Status status = bsonExtractField(cmdObj, saslCommandPayloadFieldName, &payloadElement);
    if (!status.isOK())
        return status;

    *type = payloadElement.type();
    if (payloadElement.type() == BinData) {
        const char* payloadData;
        int payloadLen;
        payloadData = payloadElement.binData(payloadLen);
        if (payloadLen < 0)
            return Status(ErrorCodes::InvalidLength, "Negative payload length");
        *payload = std::string(payloadData, payloadData + payloadLen);
    } else if (payloadElement.type() == String) {
        try {
            *payload = base64::decode(payloadElement.str());
        } catch (AssertionException& e) {
            return Status(ErrorCodes::FailedToParse, e.what());
        }
    } else {
        return Status(ErrorCodes::TypeMismatch,
                      (str::stream() << "Wrong type for field; expected BinData or String for "
                                     << payloadElement));
    }

    return Status::OK();
}
}  // namespace mongo
