/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/admission/execution_admission_context.h"
#include "mongo/db/auth/authorization_checks.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/buildinfo_common.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/global_catalog/type_collection.h"
#include "mongo/db/local_catalog/coll_mod.h"
#include "mongo/db/local_catalog/ddl/coll_mod_gen.h"
#include "mongo/db/local_catalog/ddl/coll_mod_reply_validation.h"
#include "mongo/db/local_catalog/ddl/replica_set_ddl_tracker.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/profile_settings.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/timeseries/collection_pre_conditions_util.h"
#include "mongo/db/timeseries/timeseries_collmod.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/rpc/reply_builder_interface.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <set>
#include <string>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace {
class CollectionModCommand : public TypedCommand<CollectionModCommand> {
public:
    using Request = CollMod;
    using Reply = CollModReply;

    const std::set<std::string>& apiVersions() const override {
        return kApiVersions1;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool allowedWithSecurityToken() const final {
        return true;
    }

    bool collectsResourceConsumptionMetrics() const override {
        return true;
    }

    std::string help() const override {
        return "Sets collection options.\n"
               "Example: { collMod: 'foo', viewOn: 'bar'} "
               "Example: { collMod: 'foo', index: {keyPattern: {a: 1}, expireAfterSeconds: 600} "
               "Example: { collMod: 'foo', index: {name: 'bar', expireAfterSeconds: 600} }\n";
    }

    const AuthorizationContract* getAuthorizationContract() const final {
        return &Request::kAuthorizationContract;
    }

    class Invocation final : public MinimalInvocationBase {
    public:
        using MinimalInvocationBase::MinimalInvocationBase;
        bool supportsWriteConcern() const override {
            return true;
        }

        NamespaceString ns() const final {
            return request().getNamespace();
        }

        bool isSubjectToIngressAdmissionControl() const override {
            return true;
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            uassertStatusOK(auth::checkAuthForCollMod(opCtx,
                                                      AuthorizationSession::get(opCtx->getClient()),
                                                      request().getNamespace(),
                                                      unparsedRequest().body,
                                                      false,
                                                      request().getSerializationContext()));
        }

        void run(OperationContext* opCtx, rpc::ReplyBuilderInterface* reply) final {
            const auto& cmd = request();
            const auto& nss = request().getNamespace();

            ReplicaSetDDLTracker::ScopedReplicaSetDDL scopedReplicaSetDDL(
                opCtx, std::vector<NamespaceString>{nss});

            staticValidateCollMod(opCtx, nss, cmd.getCollModRequest());

            // Updating granularity on sharded time-series collections is not allowed.
            // TODO SERVER-105548 remove completely this check once 9.0 becomes last LTS
            auto catalogClient =
                Grid::get(opCtx)->isInitialized() ? Grid::get(opCtx)->catalogClient() : nullptr;
            if (catalogClient && cmd.getTimeseries() && cmd.getTimeseries()->getGranularity()) {
                auto preConditions =
                    timeseries::CollectionPreConditions::getCollectionPreConditions(
                        opCtx, nss, /*expectedUUID=*/boost::none);
                try {
                    auto coll = catalogClient->getCollection(opCtx, preConditions.getTargetNs(nss));
                    uassert(ErrorCodes::NotImplemented,
                            str::stream()
                                << "Cannot update granularity of a sharded time-series collection.",
                            !coll.getTimeseriesFields());
                } catch (const ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
                    // Collection is not sharded, skip check.
                }
            }

            if (cmd.getValidator() || cmd.getValidationLevel() || cmd.getValidationAction()) {
                // Check for config.settings in the user command since a validator is allowed
                // internally on this collection but the user may not modify the validator.
                uassert(ErrorCodes::InvalidOptions,
                        str::stream() << "Document validators not allowed on system collection "
                                      << nss.toStringForErrorMsg(),
                        nss != NamespaceString::kConfigSettingsNamespace);
            }
            if (cmd.getValidationAction() == ValidationActionEnum::errorAndLog) {
                uassert(
                    ErrorCodes::InvalidOptions,
                    "Validation action 'errorAndLog' is not supported with current FCV",
                    gFeatureFlagErrorAndLogValidationAction.isEnabledUseLastLTSFCVWhenUninitialized(
                        VersionContext::getDecoration(opCtx),
                        serverGlobalParams.featureCompatibility.acquireFCVSnapshot()));
            }

            // We do not use the serialization context for reply object serialization as the reply
            // object doesn't contain any nss or dbName structures.
            auto result = reply->getBodyBuilder();
            uassertStatusOK(timeseries::processCollModCommandWithTimeSeriesTranslation(
                opCtx, nss, cmd, true, &result));

            // Only validate results in test mode so that we don't expose users to errors if we
            // construct an invalid reply.
            if (getTestCommandsEnabled()) {
                validateResult(result.asTempObj());
            }
        }

        void validateResult(const BSONObj& resultObj) {
            auto reply = Reply::parse(resultObj, IDLParserContext("CollModReply"));
            coll_mod_reply_validation::validateReply(reply);
        }
    };
};
MONGO_REGISTER_COMMAND(CollectionModCommand).forShard();

}  // namespace
}  // namespace mongo
