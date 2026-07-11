// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/client/sasl_plain_client_conversation.h"

#include "mongo/base/status_with.h"
#include "mongo/bson/util/builder.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/client/sasl_client_session.h"

#include <string_view>

#include <boost/move/utility_core.hpp>

namespace mongo {

SaslPLAINClientConversation::SaslPLAINClientConversation(SaslClientSession* saslClientSession)
    : SaslClientConversation(saslClientSession) {}

SaslPLAINClientConversation::~SaslPLAINClientConversation() {};

StatusWith<bool> SaslPLAINClientConversation::step(std::string_view inputData,
                                                   std::string* outputData) {
    // Create PLAIN message on the form: user\0user\0pwd

    StringBuilder sb;
    sb << std::string{_saslClientSession->getParameter(SaslClientSession::parameterUser)} << '\0'
       << std::string{_saslClientSession->getParameter(SaslClientSession::parameterUser)} << '\0'
       << std::string{_saslClientSession->getParameter(SaslClientSession::parameterPassword)};

    *outputData = sb.str();

    return StatusWith<bool>(true);
}

}  // namespace mongo
