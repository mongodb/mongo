// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/error_codes.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/rotate_certificates_gen.h"
#include "mongo/db/database_name.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/net/ssl_manager.h"

#include <memory>
#include <string>

#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {
namespace {

class RotateCertificatesCmd final : public TypedCommand<RotateCertificatesCmd> {
public:
    using Request = RotateCertificates;

    std::string help() const override {
        return "Rotate certificates for new SSL connections";
    }

    bool adminOnly() const override {
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
#ifdef MONGO_CONFIG_SSL
            if (SSLManagerCoordinator::get()) {
                SSLManagerCoordinator::get()->rotate();
            }
            auto messageArg = request().getMessage();
            auto message = messageArg.get_value_or("");
            LOGV2(4988500, "Certificate rotation completed successfully", "message"_attr = message);

#endif
        }

        NamespaceString ns() const override {
            return NamespaceString::kEmpty;
        }

        bool supportsWriteConcern() const override {
            return false;
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForActionsOnResource(
                            ResourcePattern::forClusterResource(request().getDbName().tenantId()),
                            ActionType::rotateCertificates));
        }
    };
};
MONGO_REGISTER_COMMAND(RotateCertificatesCmd).forRouter().forShard();

}  // namespace
}  // namespace mongo
