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

#include "mongo/platform/basic.h"

#include "mongo/client/sasl_oidc_client_conversation.h"

#include "mongo/client/sasl_client_session.h"
#include "mongo/client/sasl_oidc_client_params.h"
#include "mongo/db/auth/oidc_protocol_gen.h"

namespace mongo {

OIDCClientGlobalParams oidcClientGlobalParams;

StatusWith<bool> SaslOIDCClientConversation::step(StringData inputData, std::string* outputData) {
    switch (++_step) {
        case 1:
            return _firstStep(outputData);
        case 2:
            return _secondStep(inputData, outputData);
        default:
            return StatusWith<bool>(ErrorCodes::AuthenticationFailed,
                                    str::stream()
                                        << "Invalid client OIDC authentication step: " << _step);
    }
}

StatusWith<bool> SaslOIDCClientConversation::_firstStep(std::string* outputData) {
    // If an access token was provided without a username, proceed to the second step and send it
    // directly to the server.
    if (_principalName.empty()) {
        if (_accessToken.empty()) {
            return Status(ErrorCodes::AuthenticationFailed,
                          "Either a username or an access token must be provided for the "
                          "MONGODB-OIDC mechanism");
        }
        try {
            auto ret = _secondStep("", outputData);
            ++_step;
            return ret;
        } catch (const DBException& ex) {
            return ex.toStatus();
        }
    }

    // If the username is provided, then request information needed to contact the identity provider
    // from the server.
    auth::OIDCMechanismClientStep1 firstClientRequest;
    firstClientRequest.setPrincipalName(StringData(_principalName));
    auto firstClientRequestBSON = firstClientRequest.toBSON();
    *outputData = std::string(firstClientRequestBSON.objdata(), firstClientRequestBSON.objsize());
    return false;
}

StatusWith<bool> SaslOIDCClientConversation::_secondStep(StringData input,
                                                         std::string* outputData) {
    // If the client already has a non-empty access token, then token acquisition can be skipped.
    if (_accessToken.empty()) {
        // TODO SERVER-70958: Implement device authorization grant flow to acquire token.
        uasserted(ErrorCodes::NotImplemented,
                  "TODO: SERVER-70958 Implement device authorization grant flow to acquire token");
    }

    auth::OIDCMechanismClientStep2 secondClientRequest;
    secondClientRequest.setJWT(_accessToken);
    auto bson = secondClientRequest.toBSON();
    *outputData = std::string(bson.objdata(), bson.objsize());

    return true;
}

}  // namespace mongo
