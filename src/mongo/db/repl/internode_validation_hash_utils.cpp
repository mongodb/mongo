// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/repl/internode_validation_hash_utils.h"

#include "mongo/db/server_feature_flags_gen.h"

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

bool isContinuousInternodeValidationPerDocumentEnabled(OperationContext* opCtx) {
    const auto fcvSnapshot = serverGlobalParams.featureCompatibility.acquireFCVSnapshot();
    const auto& vCtx = VersionContext::getDecoration(opCtx);
    return gFeatureFlagContinuousInternodeValidationPerDocument
        .isEnabledUseLatestFCVWhenUninitialized(vCtx, fcvSnapshot);
}
}  // namespace repl
}  // namespace mongo
