// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/repl/internode_validation_hash_utils.h"

#include "mongo/db/feature_flag.h"
#include "mongo/db/repl/repl_server_parameters_gen.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/rss/replicated_storage_service.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/version_context.h"

// The //src/third_party/xxhash target is header-only and defines XXH_INLINE_ALL for its consumers,
// so XXH3 is inlined into the hashing functions below.
#include <xxhash.h>

namespace mongo {
namespace repl {

int64_t computeDocValidationHash(const BSONObj& doc) {
    // XXH3 is a non-cryptographic hash, so this value is only used to guard against replication
    // bugs and storage corruption.
    return static_cast<int64_t>(XXH3_64bits(doc.objdata(), doc.objsize()));
}

int64_t computeUpdateValidationHash(const BSONObj& preImage, const BSONObj& postImage) {
    return computeDocValidationHash(preImage) ^ computeDocValidationHash(postImage);
}

namespace {
bool isPerDocumentFlagEnabled(const VersionContext& vCtx,
                              const ServerGlobalParams::FCVSnapshot& fcvSnapshot) {
    return gFeatureFlagContinuousInternodeValidationPerDocument
        .isEnabledUseLatestFCVWhenUninitialized(vCtx, fcvSnapshot);
}

/**
 * The persistence provider decides whether continuous internode validation is part of the
 * storage model, in which case it is enabled independently of the FCV-gated feature flags.
 */
bool providerUsesContinuousInternodeValidation(OperationContext* opCtx) {
    return rss::ReplicatedStorageService::get(opCtx)
        .getPersistenceProvider()
        .shouldUseContinuousInternodeValidation();
}

/**
 * Validation hashes are carried on oplog entries and verified as those entries are applied, so
 * they are only meaningful on a replica set member.
 */
bool isReplSet(OperationContext* opCtx) {
    return ReplicationCoordinator::get(opCtx)->getSettings().isReplSet();
}

/**
 * Emergency override, checked ahead of both the persistence provider and the feature flags. It is
 * settable at runtime: a validation hash is only ever verified against the entry that carries it,
 * so entries written while validation was disabled are skipped by the applier rather than reported
 * as mismatches, and toggling this has no effect beyond the operations it spans.
 */
bool isDisabledByOverride() {
    return disableContinuousInternodeValidation.load();
}
}  // namespace

bool isContinuousInternodeValidationPerDocumentEnabled(OperationContext* opCtx) {
    if (MONGO_unlikely(isDisabledByOverride() || !isReplSet(opCtx))) {
        return false;
    }

    // TODO(SERVER-133384): Remove feature flag check.
    return providerUsesContinuousInternodeValidation(opCtx) ||
        isPerDocumentFlagEnabled(VersionContext::getDecoration(opCtx),
                                 serverGlobalParams.featureCompatibility.acquireFCVSnapshot());
}

bool isContinuousInternodeValidationPerCollectionEnabled(OperationContext* opCtx) {
    if (MONGO_unlikely(isDisabledByOverride() || !isReplSet(opCtx))) {
        return false;
    }

    if (providerUsesContinuousInternodeValidation(opCtx)) {
        return true;
    }

    // Both flags need to be checked against the same FCV snapshot.
    const auto fcvSnapshot = serverGlobalParams.featureCompatibility.acquireFCVSnapshot();
    const auto& vCtx = VersionContext::getDecoration(opCtx);

    // TODO(SERVER-133384): Remove feature flag check.
    // The collection hash is folded from the per-document hashes carried on the oplog entries, so
    // it requires per-document validation to be enabled as well.
    return isPerDocumentFlagEnabled(vCtx, fcvSnapshot) &&
        gFeatureFlagContinuousInternodeValidationPerCollection
            .isEnabledUseLatestFCVWhenUninitialized(vCtx, fcvSnapshot);
}
}  // namespace repl
}  // namespace mongo
