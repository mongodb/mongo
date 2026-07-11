// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/client/sasl_client_authenticate.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/auth/sasl_command_constants.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/base64.h"
#include "mongo/util/str.h"

#include <string>

#include <boost/move/utility_core.hpp>

namespace mongo {

Future<void> (*saslClientAuthenticate)(auth::RunCommandHook runCommand,
                                       const HostAndPort& hostname,
                                       const auth::Credential& credential) = nullptr;

Status saslExtractPayload(const BSONObj& cmdObj, std::string* payload, BSONType* type) {
    BSONElement payloadElement;
    Status status = bsonExtractField(cmdObj, saslCommandPayloadFieldName, &payloadElement);
    if (!status.isOK())
        return status;

    *type = payloadElement.type();
    if (payloadElement.type() == BSONType::binData) {
        const char* payloadData;
        int payloadLen;
        payloadData = payloadElement.binData(payloadLen);
        if (payloadLen < 0)
            return Status(ErrorCodes::InvalidLength, "Negative payload length");
        *payload = std::string(payloadData, payloadData + payloadLen);
    } else if (payloadElement.type() == BSONType::string) {
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
