// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/commands/set_feature_compatibility_version_steps/fcv_step.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/query_feature_flags_gen.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/db/shard_role/shard_catalog/collection_catalog_helper.h"
#include "mongo/db/shard_role/shard_catalog/drop_collection.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"

#include <string>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQueryCE

namespace mongo {
namespace {

MONGO_FAIL_POINT_DEFINE(hangBeforeDroppingPersistedSamples);

/**
 * Drops the persisted samples collection when the FCV is downgraded to a version where
 * featureFlagPersistentStats is disabled.
 */
class PersistentStatsFCVStep : public FCVStep {
public:
    static PersistentStatsFCVStep* get(ServiceContext* serviceContext);

    std::string getStepName() const final {
        return "PersistentStatsFCVStep";
    }

private:
    void internalServerCleanupForDowngrade(OperationContext* opCtx,
                                           FCV originalVersion,
                                           FCV requestedVersion) final {

        const bool shouldMigrate =
            feature_flags::gFeatureFlagPersistentStats.isDisabledOnTargetFCVButEnabledOnOriginalFCV(
                requestedVersion, originalVersion);
        if (!shouldMigrate) {
            return;
        }

        if (MONGO_unlikely(hangBeforeDroppingPersistedSamples.shouldFail())) {
            hangBeforeDroppingPersistedSamples.pauseWhileSet();
        }

        catalog::modifyAllCollectionsMatching(
            opCtx,
            [&](const Collection* collection) {
                const NamespaceString nss = collection->ns();
                DropReply reply;
                uassertStatusOK(dropCollection(
                    opCtx,
                    nss,
                    &reply,
                    DropCollectionSystemCollectionMode::kAllowSystemCollectionDrops));
                LOGV2(12433600, "Dropped persisted samples collection", "namespace"_attr = nss);
            },
            [](const auto* collection) { return collection->ns().isStatsSamplesCollection(); });
    }
};

const auto decoration = ServiceContext::declareDecoration<PersistentStatsFCVStep>();
const FCVStepRegistry::Registerer<PersistentStatsFCVStep> persistentStatsFCVStepRegisterer(
    "PersistentStatsFCVStep");

PersistentStatsFCVStep* PersistentStatsFCVStep::get(ServiceContext* serviceContext) {
    return &decoration(serviceContext);
}

}  // namespace
}  // namespace mongo
