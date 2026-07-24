// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/repl/internode_validation_hash_utils.h"

#include "mongo/base/data_range.h"
#include "mongo/base/data_type_endian.h"
#include "mongo/base/data_view.h"
#include "mongo/crypto/hash_block.h"
#include "mongo/crypto/sha256_block.h"
#include "mongo/db/server_feature_flags_gen.h"

namespace mongo {
namespace repl {

int64_t computeDocValidationHash(const BSONObj& doc) {
    // Reuse a single EVP_MD_CTX per thread across all documents this thread hashes, rather than
    // allocating one per operation.
    thread_local HashContext ctx;
    auto sha =
        SHA256Block::computeHashWithCtx(&ctx, {ConstDataRange(doc.objdata(), doc.objsize())});
    return ConstDataView(reinterpret_cast<const char*>(sha.data())).read<LittleEndian<int64_t>>();
}

bool isContinuousInternodeValidationPerDocumentEnabled(OperationContext* opCtx) {
    const auto fcvSnapshot = serverGlobalParams.featureCompatibility.acquireFCVSnapshot();
    const auto& vCtx = VersionContext::getDecoration(opCtx);
    return gFeatureFlagContinuousInternodeValidationPerDocument
        .isEnabledUseLatestFCVWhenUninitialized(vCtx, fcvSnapshot);
}
}  // namespace repl
}  // namespace mongo
