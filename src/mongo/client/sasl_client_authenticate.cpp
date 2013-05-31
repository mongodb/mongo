/*    Copyright 2012 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#include "mongo/client/sasl_client_authenticate.h"

#include <string>

#include "mongo/base/string_data.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/util/base64.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

    using namespace mongoutils;

    Status (*saslClientAuthenticate)(DBClientWithCommands* client,
                                     const BSONObj& saslParameters) = NULL;

    const char* const saslStartCommandName = "saslStart";
    const char* const saslContinueCommandName = "saslContinue";
    const char* const saslCommandAutoAuthorizeFieldName = "autoAuthorize";
    const char* const saslCommandCodeFieldName = "code";
    const char* const saslCommandConversationIdFieldName = "conversationId";
    const char* const saslCommandDoneFieldName = "done";
    const char* const saslCommandErrmsgFieldName = "errmsg";
    const char* const saslCommandMechanismFieldName = "mechanism";
    const char* const saslCommandMechanismListFieldName = "supportedMechanisms";
    const char* const saslCommandPasswordFieldName = "pwd";
    const char* const saslCommandPayloadFieldName = "payload";
    const char* const saslCommandUserFieldName = "user";
    const char* const saslCommandUserSourceFieldName = "userSource";
    const char* const saslCommandServiceHostnameFieldName = "serviceHostname";
    const char* const saslCommandServiceNameFieldName = "serviceName";
    const char* const saslCommandDigestPasswordFieldName = "digestPassword";
    const char* const saslDefaultDBName = "$external";
    const char* const saslDefaultServiceName = "mongodb";

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
        }
        else if (payloadElement.type() == String) {
            try {
                *payload = base64::decode(payloadElement.str());
            } catch (UserException& e) {
                return Status(ErrorCodes::FailedToParse, e.what());
            }
        }
        else {
            return Status(ErrorCodes::TypeMismatch,
                          (str::stream() << "Wrong type for field; expected BinData or String for "
                           << payloadElement));
        }

        return Status::OK();
    }
}  // namespace mongo
