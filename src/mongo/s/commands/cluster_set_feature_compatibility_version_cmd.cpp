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

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/set_feature_compatibility_version_gen.h"
#include "mongo/db/database_name.h"
#include "mongo/db/feature_compatibility_version_documentation.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/topology/shard_registry.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/database_name_util.h"
#include "mongo/util/version/releases.h"

#include <memory>
#include <ostream>
#include <string>

namespace mongo {

namespace {

/**
 * Sets the minimum allowed version for the cluster. If it is the last stable
 * featureCompatibilityVersion, then shards will not use latest featureCompatibilityVersion
 * features.
 *
 * Format:
 * {
 *   setFeatureCompatibilityVersion: <string version>
 * }
 */
class SetFeatureCompatibilityVersionCmd final
    : public TypedCommand<SetFeatureCompatibilityVersionCmd> {
public:
    using Request = SetFeatureCompatibilityVersion;
    using GenericFCV = multiversion::GenericFCV;

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool adminOnly() const override {
        return true;
    }

    std::string help() const override {
        std::stringstream h;
        h << "Set the featureCompatibilityVersion used by this cluster. If set to '"
          << multiversion::toString(GenericFCV::kLastLTS)
          << "', then features introduced in versions greater than '"
          << multiversion::toString(GenericFCV::kLastLTS) << "' will be disabled";
        if (GenericFCV::kLastContinuous != GenericFCV::kLastLTS) {
            h << " If set to '" << multiversion::toString(GenericFCV::kLastContinuous)
              << "', then features introduced in '" << multiversion::toString(GenericFCV::kLatest)
              << "' will be disabled.";
        }
        h << " If set to '" << multiversion::toString(GenericFCV::kLatest) << "', then '"
          << multiversion::toString(GenericFCV::kLatest)
          << "' features are enabled, and all nodes in the cluster must be binary version "
          << multiversion::toString(GenericFCV::kLatest) << ". See "
          << feature_compatibility_version_documentation::compatibilityLink() << ".";
        return h.str();
    }

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            auto cmd = request();
            generic_argument_util::setMajorityWriteConcern(cmd, &opCtx->getWriteConcern());

            // Forward to config shard, which will forward to all shards.
            auto configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();
            auto response = uassertStatusOK(configShard->runCommand(
                opCtx,
                ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                cmd.getDbName(),
                CommandHelpers::filterCommandRequestForPassthrough(cmd.toBSON()),
                Shard::RetryPolicy::kIdempotent));
            uassertStatusOK(response.commandStatus);
        }

        NamespaceString ns() const override {
            return NamespaceString::kEmpty;
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForActionsOnResource(
                            ResourcePattern::forClusterResource(request().getDbName().tenantId()),
                            ActionType::setFeatureCompatibilityVersion));
        }

        bool supportsWriteConcern() const override {
            return true;
        }
    };
};
MONGO_REGISTER_COMMAND(SetFeatureCompatibilityVersionCmd).forRouter();

}  // namespace
}  // namespace mongo
