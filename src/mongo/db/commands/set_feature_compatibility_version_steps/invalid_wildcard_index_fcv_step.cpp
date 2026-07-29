// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/commands/set_feature_compatibility_version_steps/fcv_step.h"
#include "mongo/db/index/wildcard_validation.h"
#include "mongo/db/index_names.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/db/shard_role/shard_catalog/collection_catalog_helper.h"

#include <fmt/format.h>

namespace mongo {
namespace {

/*
 * Blocks FCV upgrades across the 9.0 boundary while a compound wildcard index with an invalid
 * wildcardProjection exists. Such indexes could be created before SERVER-113685 banned them, and
 * using them to answer queries can lead to undefined behavior. Requiring users to drop them before
 * upgrading guarantees they no longer exist once the FCV reaches 9.0.
 *
 * TODO (SERVER-132386): Remove this step once 10.0 becomes last LTS.
 */
class InvalidWildcardIndexFCVStep : public FCVStep {
public:
    static InvalidWildcardIndexFCVStep* get(ServiceContext* serviceContext);

    std::string getStepName() const final {
        return "InvalidWildcardIndexFCVStep";
    }

private:
    void userCollectionsUassertsForUpgrade(OperationContext* opCtx,
                                           FCV originalVersion,
                                           FCV requestedVersion) final {
        if (requestedVersion < FCV::kVersion_9_0) {
            return;
        }

        catalog::forEachCollectionFromAllDbs(opCtx, MODE_IS, [](const Collection* collection) {
            std::vector<std::string> indexNames;
            collection->getAllIndexes(&indexNames);

            for (const auto& indexName : indexNames) {
                const BSONObj spec = collection->getIndexSpec(indexName);
                const BSONObj keyPattern = spec.getObjectField("key");

                if (IndexNames::findPluginName(keyPattern) != IndexNames::WILDCARD ||
                    keyPattern.nFields() <= 1 || !spec.hasField("wildcardProjection")) {
                    continue;
                }

                auto validationStatus = validateWildcardProjection(
                    keyPattern, spec.getObjectField("wildcardProjection"));
                if (!validationStatus.isOK()) {
                    uasserted(ErrorCodes::CannotUpgrade,
                              fmt::format("Cannot upgrade the cluster because collection {} "
                                          "(UUID: {}) has a compound wildcard index '{}' with an "
                                          "invalid wildcardProjection: {}. Such indexes can no "
                                          "longer be created. Please drop the index before "
                                          "upgrading.",
                                          collection->ns().toStringForErrorMsg(),
                                          collection->uuid().toString(),
                                          indexName,
                                          validationStatus.reason()));
                }
            }

            return true;
        });
    }
};

const auto decoration = ServiceContext::declareDecoration<InvalidWildcardIndexFCVStep>();
const FCVStepRegistry::Registerer<InvalidWildcardIndexFCVStep>
    invalidWildcardIndexFCVStepRegisterer("InvalidWildcardIndexFCVStep");

InvalidWildcardIndexFCVStep* InvalidWildcardIndexFCVStep::get(ServiceContext* serviceContext) {
    return &decoration(serviceContext);
}

}  // namespace
}  // namespace mongo
