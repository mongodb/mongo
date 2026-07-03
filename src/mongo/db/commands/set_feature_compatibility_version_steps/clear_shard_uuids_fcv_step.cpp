/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/commands/set_feature_compatibility_version_steps/fcv_step.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/global_catalog/type_shard.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/shard_role/ddl/drop_indexes_gen.h"
#include "mongo/db/sharding_environment/sharding_feature_flags_gen.h"
#include "mongo/db/topology/sharding_state.h"

namespace mongo {
namespace {

/*
 * Executes the necessary steps to remove any shard UUID value from any persisted metadata on shard
 * and config catalogs.
 */
class ClearShardUuidsFCVStep : public FCVStep {
public:
    static ClearShardUuidsFCVStep* get(ServiceContext* serviceContext);

    inline std::string getStepName() const final {
        return "ClearShardUuidsFCVStep";
    }

private:
    void finalizeDowngrade(OperationContext* opCtx, FCV requestedVersion) final {
        const auto role = ShardingState::get(opCtx)->pollClusterRole();
        if (!role || !role->has(ClusterRole::ConfigServer)) {
            return;
        }
        if (feature_flags::gFeatureFlagUniqueShardIdentifiers.isEnabledOnVersion(
                requestedVersion)) {
            return;
        }
        try {
            DropIndexes dropIndexesCmd{NamespaceString::kConfigsvrShardsNamespace};
            dropIndexesCmd.setIndex(BSON(ShardType::uuid() << 1));
            BSONObj info;
            DBDirectClient client{opCtx};
            if (!client.runCommand(NamespaceString::kConfigsvrShardsNamespace.dbName(),
                                   dropIndexesCmd.toBSON(),
                                   info)) {
                uassertStatusOK(getStatusFromCommandResult(info));
            }
        } catch (const ExceptionFor<ErrorCodes::IndexNotFound>&) {
        } catch (const ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
        }
    }
};

const auto decoration = ServiceContext::declareDecoration<ClearShardUuidsFCVStep>();
const FCVStepRegistry::Registerer<ClearShardUuidsFCVStep> clearShardUuidFCVStepRegisterer(
    "ClearShardUuidsFCVStep");

ClearShardUuidsFCVStep* ClearShardUuidsFCVStep::get(ServiceContext* serviceContext) {
    return &decoration(serviceContext);
}

}  // namespace
}  // namespace mongo
