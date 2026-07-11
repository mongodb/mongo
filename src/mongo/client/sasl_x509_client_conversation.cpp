// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/client/sasl_x509_client_conversation.h"

#include "mongo/base/status_with.h"
#include "mongo/client/sasl_client_session.h"
#include "mongo/db/auth/x509_protocol_gen.h"

#include <string_view>

namespace mongo {

SaslX509ClientConversation::SaslX509ClientConversation(SaslClientSession* saslClientSession)
    : SaslClientConversation(saslClientSession) {}

StatusWith<bool> SaslX509ClientConversation::step(std::string_view inputData,
                                                  std::string* outputData) {
    // Create an X509 message in the form of BSON("username": user)

    auth::X509MechanismClientStep1 step;
    if (auto principal = _saslClientSession->getParameter(SaslClientSession::parameterUser);
        !principal.empty()) {
        step.setPrincipalName(principal);
    }
    auto stepBSON = step.toBSON();

    *outputData = std::string{std::string_view(stepBSON.objdata(), stepBSON.objsize())};

    return StatusWith<bool>(true);
}

}  // namespace mongo
