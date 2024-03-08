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

#include <absl/container/node_hash_map.h>
#include <tuple>

#include "mongo/base/error_codes.h"
#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/initializer.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/authenticate.h"
#include "mongo/client/native_sasl_client_session.h"
#include "mongo/client/sasl_client_conversation.h"
#include "mongo/client/sasl_oidc_client_conversation.h"
#include "mongo/client/sasl_plain_client_conversation.h"
#include "mongo/client/sasl_scram_client_conversation.h"
#include "mongo/client/scram_client_cache.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/crypto/sha1_block.h"
#include "mongo/crypto/sha256_block.h"
#include "mongo/db/commands/server_status.h"
#include "mongo/db/operation_context.h"
#include "mongo/util/str.h"

#ifdef MONGO_CONFIG_SSL
#include "mongo/client/sasl_aws_client_conversation.h"
#endif

namespace mongo {
namespace {

SaslClientSession* createNativeSaslClientSession(const std::string mech) {
    return new NativeSaslClientSession();
}

MONGO_INITIALIZER(NativeSaslClientContext)(InitializerContext* context) {
    SaslClientSession::create = createNativeSaslClientSession;
}

// Global cache for SCRAM-SHA-1/256 credentials
auto* scramsha1ClientCache = new SCRAMClientCache<SHA1Block>;
auto* scramsha256ClientCache = new SCRAMClientCache<SHA256Block>;

template <typename HashBlock>
void cacheToBSON(SCRAMClientCache<HashBlock>* cache, StringData name, BSONObjBuilder* builder) {
    auto stats = cache->getStats();

    BSONObjBuilder sub(builder->subobjStart(name));
    sub.append("count", stats.count);
    sub.append("hits", stats.hits);
    sub.append("misses", stats.misses);
}

/**
 * Output stats about the SCRAM client cache to server status.
 */
class ScramCacheStatsStatusSection : public ServerStatusSection {
public:
    using ServerStatusSection::ServerStatusSection;

    bool includeByDefault() const override {
        return true;
    }

    BSONObj generateSection(OperationContext* opCtx,
                            const BSONElement& configElement) const override {
        BSONObjBuilder builder;
        cacheToBSON(scramsha1ClientCache, "SCRAM-SHA-1", &builder);
        cacheToBSON(scramsha256ClientCache, "SCRAM-SHA-256", &builder);
        return builder.obj();
    }
};

auto& scramCacheSection = *ServerStatusSectionBuilder<ScramCacheStatsStatusSection>("scramCache");

}  // namespace

NativeSaslClientSession::NativeSaslClientSession()
    : SaslClientSession(), _step(0), _success(false), _saslConversation(nullptr) {}

NativeSaslClientSession::~NativeSaslClientSession() {}

Status NativeSaslClientSession::initialize() {
    if (_saslConversation)
        return Status(ErrorCodes::AlreadyInitialized,
                      "Cannot reinitialize NativeSaslClientSession.");

    std::string mechanism = getParameter(parameterMechanism).toString();
    if (mechanism == auth::kMechanismSaslPlain) {
        _saslConversation.reset(new SaslPLAINClientConversation(this));
    } else if (mechanism == auth::kMechanismScramSha1) {
        _saslConversation.reset(
            new SaslSCRAMClientConversationImpl<SHA1Block>(this, scramsha1ClientCache));
    } else if (mechanism == auth::kMechanismScramSha256) {
        _saslConversation.reset(
            new SaslSCRAMClientConversationImpl<SHA256Block>(this, scramsha256ClientCache));
#ifdef MONGO_CONFIG_SSL
        // AWS depends on kms-message which depends on ssl libraries
    } else if (mechanism == auth::kMechanismMongoAWS) {
        _saslConversation.reset(new SaslAWSClientConversation(this));
#endif
    } else if (mechanism == auth::kMechanismMongoOIDC) {
        auto userName = hasParameter(SaslClientSession::parameterUser)
            ? getParameter(SaslClientSession::parameterUser)
            : ""_sd;
        auto accessToken = hasParameter(SaslClientSession::parameterOIDCAccessToken)
            ? getParameter(SaslClientSession::parameterOIDCAccessToken)
            : ""_sd;

        _saslConversation.reset(new SaslOIDCClientConversation(this, userName, accessToken));
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
