// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/client/native_sasl_client_session.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/initializer.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/authenticate.h"
#include "mongo/client/sasl_client_conversation.h"
#include "mongo/client/sasl_oidc_client_conversation.h"
#include "mongo/client/sasl_plain_client_conversation.h"
#include "mongo/client/sasl_scram_client_conversation.h"
#include "mongo/client/sasl_x509_client_conversation.h"
#include "mongo/client/scram_client_cache.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/crypto/sha1_block.h"
#include "mongo/crypto/sha256_block.h"
#include "mongo/db/commands/server_status/server_status.h"
#include "mongo/db/operation_context.h"
#include "mongo/util/str.h"

#include <string_view>
#include <tuple>

#include <absl/container/node_hash_map.h>

#ifdef MONGO_CONFIG_SSL
#include "mongo/client/sasl_aws_client_conversation.h"
#endif

namespace mongo {
using namespace std::literals::string_view_literals;
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
void cacheToBSON(SCRAMClientCache<HashBlock>* cache,
                 std::string_view name,
                 BSONObjBuilder* builder) {
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
        cacheToBSON(scramsha1ClientCache, auth::kMechanismScramSha1, &builder);
        cacheToBSON(scramsha256ClientCache, auth::kMechanismScramSha256, &builder);
        return builder.obj();
    }
};

auto& scramCacheSection =
    *ServerStatusSectionBuilder<ScramCacheStatsStatusSection>("scramCache").forShard().forRouter();

}  // namespace

NativeSaslClientSession::NativeSaslClientSession()
    : SaslClientSession(), _success(false), _saslConversation(nullptr) {}

NativeSaslClientSession::~NativeSaslClientSession() {}

Status NativeSaslClientSession::initialize() {
    if (_saslConversation)
        return Status(ErrorCodes::AlreadyInitialized,
                      "Cannot reinitialize NativeSaslClientSession.");

    std::string mechanism = std::string{getParameter(parameterMechanism)};
    if (mechanism == auth::kMechanismSaslPlain) {
        _saslConversation = std::make_unique<SaslPLAINClientConversation>(this);
    } else if (mechanism == auth::kMechanismScramSha1) {
        _saslConversation = std::make_unique<SaslSCRAMClientConversationImpl<SHA1Block>>(
            this, scramsha1ClientCache);
    } else if (mechanism == auth::kMechanismScramSha256) {
        _saslConversation = std::make_unique<SaslSCRAMClientConversationImpl<SHA256Block>>(
            this, scramsha256ClientCache);
#ifdef MONGO_CONFIG_SSL
        // AWS depends on kms-message which depends on ssl libraries
    } else if (mechanism == auth::kMechanismMongoAWS) {
        _saslConversation = std::make_unique<SaslAWSClientConversation>(this);
    } else if (mechanism == auth::kMechanismMongoX509) {
        _saslConversation = std::make_unique<SaslX509ClientConversation>(this);
#endif
    } else if (mechanism == auth::kMechanismMongoOIDC) {
        auto userName = hasParameter(SaslClientSession::parameterUser)
            ? getParameter(SaslClientSession::parameterUser)
            : ""sv;
        auto accessToken = hasParameter(SaslClientSession::parameterOIDCAccessToken)
            ? getParameter(SaslClientSession::parameterOIDCAccessToken)
            : ""sv;

        _saslConversation =
            std::make_unique<SaslOIDCClientConversation>(this, userName, accessToken);
    } else {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "SASL mechanism " << mechanism << " is not supported");
    }

    return Status::OK();
}

Status NativeSaslClientSession::step(std::string_view inputData, std::string* outputData) {
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

boost::optional<std::uint32_t> NativeSaslClientSession::currentStep() const {
    return _saslConversation->currentStep();
}

boost::optional<std::uint32_t> NativeSaslClientSession::totalSteps() const {
    return _saslConversation->totalSteps();
}
}  // namespace mongo
