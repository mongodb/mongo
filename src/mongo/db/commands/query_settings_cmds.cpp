/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/base/shim.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/cluster_server_parameter_cmds_gen.h"
#include "mongo/db/commands/query_settings_cmds_gen.h"
#include "mongo/db/commands/set_cluster_parameter_invocation.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/query_settings_cluster_parameter_gen.h"
#include "mongo/db/query/query_settings_gen.h"
#include "mongo/db/query/query_settings_manager.h"
#include "mongo/db/query/query_shape.h"
#include "mongo/platform/basic.h"
#include "mongo/stdx/variant.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace {

using namespace query_settings;

static constexpr auto kQuerySettingsClusterParameterName = "querySettings"_sd;

SetClusterParameter makeSetClusterParameterRequest(
    const std::vector<QueryShapeConfiguration>& settingsArray, const mongo::DatabaseName& dbName) {
    BSONObjBuilder bob;
    BSONArrayBuilder arrayBuilder(
        bob.subarrayStart(QuerySettingsClusterParameterValue::kSettingsArrayFieldName));
    for (const auto& item : settingsArray) {
        arrayBuilder.append(item.toBSON());
    }
    arrayBuilder.done();
    SetClusterParameter setClusterParameterRequest(
        BSON(kQuerySettingsClusterParameterName << bob.done()));

    // NOTE: Forward the 'dbName' for the SetClusterParameter::toBSON() not to fail on
    // the invariant.
    setClusterParameterRequest.setDbName(dbName);
    return setClusterParameterRequest;
}

/**
 * Invokes the setClusterParameter() weak function, which is an abstraction over the corresponding
 * command implementation in sharded clusters (mongos) vs. replica set deployments (mongod).
 */
void setClusterParameter(OperationContext* opCtx, const SetClusterParameter& request) {
    static auto w = MONGO_WEAK_FUNCTION_DEFINITION(setClusterParameter);
    w(opCtx, request);
}

class SetQuerySettingsCommand final : public TypedCommand<SetQuerySettingsCommand> {
public:
    using Request = SetQuerySettingsCommandRequest;

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool adminOnly() const override {
        return true;
    }

    std::string help() const override {
        return "Sets the query settings for the query shape of a given query.";
    }

    bool allowedWithSecurityToken() const final {
        return true;
    }

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        SetQuerySettingsCommandReply insertQuerySettings(
            OperationContext* opCtx, QueryShapeConfiguration queryShapeConfiguration) {
            // TODO: SERVER-77466 Implement validation rules for setQuerySettings command.

            // Build the new 'settingsArray' by appending 'newConfig' to the list of all
            // QueryShapeConfigurations for the given tenant.
            auto& querySettingsManager = QuerySettingsManager::get(opCtx);
            auto settingsArray = querySettingsManager.getAllQueryShapeConfigurations(
                opCtx, request().getDbName().tenantId());
            settingsArray.push_back(queryShapeConfiguration);

            // Run SetClusterParameter command with the new value of the 'querySettings' cluster
            // parameter.
            setClusterParameter(
                opCtx, makeSetClusterParameterRequest(settingsArray, request().getDbName()));
            SetQuerySettingsCommandReply reply;
            reply.setQueryShapeConfiguration(std::move(queryShapeConfiguration));
            return reply;
        }

        SetQuerySettingsCommandReply updateQuerySettings(
            OperationContext* opCtx,
            QueryShapeConfiguration currentQueryShapeConfiguration,
            QuerySettings newQuerySettings) {
            // TODO: SERVER-77465 Implement setQuerySettings command (update case).
            uasserted(ErrorCodes::NotImplemented,
                      "setQuerySettings command can not update query settings yet");
        }

        SetQuerySettingsCommandReply setQuerySettingsByQueryShapeHash(
            OperationContext* opCtx, const query_shape::QueryShapeHash& queryShapeHash) {
            auto& querySettingsManager = QuerySettingsManager::get(opCtx);
            auto tenantId = request().getDbName().tenantId();
            auto querySettings = querySettingsManager.getQuerySettingsForQueryShapeHash(
                opCtx, queryShapeHash, tenantId);
            uassert(7746401,
                    "New query settings can only be created with a query instance, but a query "
                    "hash was given.",
                    querySettings.has_value());
            return updateQuerySettings(opCtx,
                                       QueryShapeConfiguration(queryShapeHash,
                                                               std::move(querySettings->first),
                                                               std::move(querySettings->second)),
                                       std::move(request().getSettings()));
        }

        SetQuerySettingsCommandReply setQuerySettingsByQueryInstance(
            OperationContext* opCtx, const QueryInstance& queryInstance) {
            auto& querySettingsManager = QuerySettingsManager::get(opCtx);
            auto tenantId = request().getDbName().tenantId();
            auto expCtx = make_intrusive<ExpressionContext>(opCtx, nullptr, ns());
            auto queryShape = query_shape::extractQueryShape(
                queryInstance, SerializationOptions(), std::move(expCtx), tenantId);
            auto queryShapeHash = query_shape::hash(std::move(queryShape));

            // If there is already an entry for a given QueryShapeHash, then perform
            // an update, otherwise insert.
            if (auto lookupResult = querySettingsManager.getQuerySettingsForQueryShapeHash(
                    opCtx, queryShapeHash, tenantId)) {
                return updateQuerySettings(opCtx,
                                           QueryShapeConfiguration(std::move(queryShapeHash),
                                                                   std::move(lookupResult->first),
                                                                   std::move(lookupResult->second)),
                                           std::move(request().getSettings()));
            } else {
                return insertQuerySettings(
                    opCtx,
                    QueryShapeConfiguration(std::move(queryShapeHash),
                                            std::move(request().getSettings()),
                                            queryInstance));
            }
        }

        SetQuerySettingsCommandReply typedRun(OperationContext* opCtx) {
            uassert(7746400,
                    "setQuerySettings command is unknown",
                    feature_flags::gFeatureFlagQuerySettings.isEnabled(
                        serverGlobalParams.featureCompatibility));
            return stdx::visit(OverloadedVisitor{
                                   [&](const query_shape::QueryShapeHash& queryShapeHash) {
                                       return setQuerySettingsByQueryShapeHash(opCtx,
                                                                               queryShapeHash);
                                   },
                                   [&](const QueryInstance& queryInstance) {
                                       return setQuerySettingsByQueryInstance(opCtx, queryInstance);
                                   },
                               },
                               request().getCommandParameter());
        }

    private:
        bool supportsWriteConcern() const override {
            return false;
        }

        NamespaceString ns() const override {
            return NamespaceString();
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            // TODO: SERVER-77551 Ensure only users with allowed permissions may invoke query
            // settings commands.
        }
    };
} setChangeStreamStateCommand;

class RemoveQuerySettingsCommand final : public TypedCommand<RemoveQuerySettingsCommand> {
public:
    using Request = RemoveQuerySettingsCommandRequest;

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool adminOnly() const override {
        return true;
    }

    std::string help() const override {
        return "Removes the query settings for the query shape of a given query.";
    }

    bool allowedWithSecurityToken() const final {
        return true;
    }

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            uassert(7746700,
                    "removeQuerySettings command is unknown",
                    feature_flags::gFeatureFlagQuerySettings.isEnabled(
                        serverGlobalParams.featureCompatibility));
            auto tenantId = request().getDbName().tenantId();
            auto queryShapeHash = stdx::visit(
                OverloadedVisitor{
                    [&](const query_shape::QueryShapeHash& queryShapeHash) {
                        return queryShapeHash;
                    },
                    [&](const QueryInstance& queryInstance) {
                        // Converts 'queryInstance' into QueryShapeHash, for convenient comparison
                        // during search for the matching QueryShapeConfiguration.
                        auto expCtx = make_intrusive<ExpressionContext>(opCtx, nullptr, ns());
                        auto queryShape = query_shape::extractQueryShape(
                            queryInstance, SerializationOptions(), std::move(expCtx), tenantId);
                        return query_shape::hash(std::move(queryShape));
                    },
                },
                request().getCommandParameter());
            auto& querySettingsManager = QuerySettingsManager::get(opCtx);

            // Build the new 'settingsArray' by removing the QueryShapeConfiguration with a matching
            // QueryShapeHash.
            auto settingsArray =
                querySettingsManager.getAllQueryShapeConfigurations(opCtx, tenantId);
            auto matchingQueryShapeConfigurationIt =
                std::find_if(settingsArray.begin(),
                             settingsArray.end(),
                             [&](const QueryShapeConfiguration& configuration) {
                                 return configuration.getQueryShapeHash() == queryShapeHash;
                             });
            uassert(7746701,
                    "A matching query settings entry does not exist",
                    matchingQueryShapeConfigurationIt != settingsArray.end());
            settingsArray.erase(matchingQueryShapeConfigurationIt);

            // Run SetClusterParameter command with the new value of the 'querySettings' cluster
            // parameter.
            setClusterParameter(
                opCtx, makeSetClusterParameterRequest(settingsArray, request().getDbName()));
        }

    private:
        bool supportsWriteConcern() const override {
            return false;
        }

        NamespaceString ns() const override {
            return NamespaceString();
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            // TODO: SERVER-77551 Ensure only users with allowed permissions may invoke query
            // settings commands.
        }
    };
} removeChangeStreamStateCommand;
}  // namespace
}  // namespace mongo
