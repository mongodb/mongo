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

#include "mongo/platform/basic.h"

#include "mongo/client/native_sasl_client_session.h"

#include "mongo/base/init.h"
#include "mongo/client/sasl_client_conversation.h"
#include "mongo/client/sasl_plain_client_conversation.h"
#include "mongo/client/sasl_scram_client_conversation.h"
#include "mongo/client/scram_client_cache.h"
#include "mongo/crypto/sha1_block.h"
#include "mongo/crypto/sha256_block.h"
#include "mongo/util/str.h"

namespace mongo {
namespace {

SaslClientSession* createNativeSaslClientSession(const std::string mech) {
    return new NativeSaslClientSession();
}

MONGO_INITIALIZER(NativeSaslClientContext)(InitializerContext* context) {
    SaslClientSession::create = createNativeSaslClientSession;
    return Status::OK();
}

// Global cache for SCRAM-SHA-1/256 credentials
auto* scramsha1ClientCache = new SCRAMClientCache<SHA1Block>;
auto* scramsha256ClientCache = new SCRAMClientCache<SHA256Block>;

}  // namespace

NativeSaslClientSession::NativeSaslClientSession()
    : SaslClientSession(), _step(0), _success(false), _saslConversation(nullptr) {}

NativeSaslClientSession::~NativeSaslClientSession() {}

Status NativeSaslClientSession::initialize() {
    if (_saslConversation)
        return Status(ErrorCodes::AlreadyInitialized,
                      "Cannot reinitialize NativeSaslClientSession.");

    std::string mechanism = getParameter(parameterMechanism).toString();
    if (mechanism == "PLAIN") {
        _saslConversation.reset(new SaslPLAINClientConversation(this));
    } else if (mechanism == "SCRAM-SHA-1") {
        _saslConversation.reset(
            new SaslSCRAMClientConversationImpl<SHA1Block>(this, scramsha1ClientCache));
    } else if (mechanism == "SCRAM-SHA-256") {
        _saslConversation.reset(
            new SaslSCRAMClientConversationImpl<SHA256Block>(this, scramsha256ClientCache));
    } else {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "SASL mechanism " << mechanism << " is not supported");
    }

    return Status::OK();
}

Status NativeSaslClientSession::step(StringData inputData, std::string* outputData) {
    if (!_saslConversation) {
        return Status(ErrorCodes::BadValue,
                      str::stream()
                          << "The client authentication session has not been properly initialized");
    }

    StatusWith<bool> status = _saslConversation->step(inputData, outputData);
    if (status.isOK()) {
        _success = status.getValue();
    }
    return status.getStatus();
}
}  // namespace mongo
