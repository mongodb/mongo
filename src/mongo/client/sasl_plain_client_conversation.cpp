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

#include "mongo/client/sasl_plain_client_conversation.h"

#include "mongo/base/status_with.h"
#include "mongo/bson/util/builder.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/client/sasl_client_session.h"

#include <boost/move/utility_core.hpp>

namespace mongo {

SaslPLAINClientConversation::SaslPLAINClientConversation(SaslClientSession* saslClientSession)
    : SaslClientConversation(saslClientSession) {}

SaslPLAINClientConversation::~SaslPLAINClientConversation() {};

StatusWith<bool> SaslPLAINClientConversation::step(StringData inputData, std::string* outputData) {
    // Create PLAIN message on the form: user\0user\0pwd

    StringBuilder sb;
    sb << std::string{_saslClientSession->getParameter(SaslClientSession::parameterUser)} << '\0'
       << std::string{_saslClientSession->getParameter(SaslClientSession::parameterUser)} << '\0'
       << std::string{_saslClientSession->getParameter(SaslClientSession::parameterPassword)};

    *outputData = sb.str();

    return StatusWith<bool>(true);
}

}  // namespace mongo
