/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/client/sasl_x509_client_conversation.h"

#include "mongo/base/status_with.h"
#include "mongo/client/sasl_client_session.h"
#include "mongo/db/auth/x509_protocol_gen.h"

namespace mongo {

SaslX509ClientConversation::SaslX509ClientConversation(SaslClientSession* saslClientSession)
    : SaslClientConversation(saslClientSession) {}

StatusWith<bool> SaslX509ClientConversation::step(StringData inputData, std::string* outputData) {
    // Create an X509 message in the form of BSON("username": user)

    auth::X509MechanismClientStep1 step;
    if (auto principal = _saslClientSession->getParameter(SaslClientSession::parameterUser);
        !principal.empty()) {
        step.setPrincipalName(principal);
    }
    auto stepBSON = step.toBSON();

    *outputData = std::string{StringData(stepBSON.objdata(), stepBSON.objsize())};

    return StatusWith<bool>(true);
}

}  // namespace mongo
