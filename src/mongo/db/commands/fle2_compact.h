// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/crypto/fle_crypto.h"
#include "mongo/crypto/fle_crypto_types.h"
#include "mongo/crypto/fle_stats_gen.h"
#include "mongo/db/commands/fle2_cleanup_gen.h"
#include "mongo/db/commands/fle2_compact_gen.h"
#include "mongo/db/fle_crud.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <memory>
#include <numeric>
#include <queue>
#include <string_view>
#include <vector>

namespace mongo {

struct [[MONGO_MOD_PUBLIC]] EncryptedStateCollectionsNamespaces {
    static StatusWith<EncryptedStateCollectionsNamespaces> createFromDataCollection(
        const Collection& edc);

    NamespaceString edcNss;
    NamespaceString escNss;
    NamespaceString ecocNss;
    NamespaceString ecocRenameNss;
    NamespaceString ecocLockNss;
};

using FLECleanupESCDeleteQueue = std::priority_queue<PrfBlock>;

/**
 * Validate a compact request has the right encryption tokens.
 */
[[MONGO_MOD_PUBLIC]] void validateCompactRequest(const CompactStructuredEncryptionData& request,
                                                 const Collection& edc);

/**
 * Validate a cleanup request has the right encryption tokens.
 */
[[MONGO_MOD_PUBLIC]] void validateCleanupRequest(const CleanupStructuredEncryptionData& request,
                                                 const Collection& edc);


void processFLECompactV2(OperationContext* opCtx,
                         const CompactStructuredEncryptionData& request,
                         GetTxnCallback getTxn,
                         const EncryptedStateCollectionsNamespaces& namespaces,
                         const std::shared_ptr<executor::TaskExecutor>& cancelExecutor,
                         ECStats* escStats,
                         ECOCStats* ecocStats);

FLECleanupESCDeleteQueue processFLECleanup(OperationContext* opCtx,
                                           const CleanupStructuredEncryptionData& request,
                                           GetTxnCallback getTxn,
                                           const EncryptedStateCollectionsNamespaces& namespaces,
                                           size_t pqMemoryLimit,
                                           ECStats* escStats,
                                           ECOCStats* ecocStats);

/**
 * Get all unique documents in the ECOC collection in their decrypted form.
 *
 * Used by unit tests.
 */
stdx::unordered_set<ECOCCompactionDocumentV2> getUniqueCompactionDocuments(
    FLEQueryInterface* queryImpl,
    BSONObj tokensObj,
    const NamespaceString& ecocNss,
    ECOCStats* ecocStats);


/**
 * Performs compaction of the ESC entries for the encrypted field/value pair
 * whose tokens are in the provided ECOC compaction document.
 *
 * Used by unit tests.
 */
void compactOneFieldValuePairV2(FLEQueryInterface* queryImpl,
                                HmacContext* hmacCtx,
                                const ECOCCompactionDocumentV2& ecocDoc,
                                const NamespaceString& escNss,
                                ECStats* escStats);


/**
 * Performs compaction for Range fields to add additional padding edges.
 */
void compactOneRangeFieldPad(FLEQueryInterface* queryImpl,
                             HmacContext* hmacCtx,
                             const NamespaceString& escNss,
                             std::string_view fieldPath,
                             BSONType fieldType,
                             const QueryTypeConfig& queryTypeConfig,
                             double anchorPaddingFactor,
                             std::size_t uniqueLeaves,
                             std::size_t uniqueTokens,
                             const AnchorPaddingRootToken& anchorPaddingRootToken,
                             ECStats* escStats,
                             std::size_t maxDocsPerInsert = write_ops::kMaxWriteBatchSize);

/**
 * Performs compaction for text search fields to add additional padding tags.
 */
void compactOneTextSearchFieldPad(FLEQueryInterface* queryImpl,
                                  HmacContext* hmacCtx,
                                  const NamespaceString& escNss,
                                  std::string_view fieldPath,
                                  std::size_t totalMsize,
                                  std::size_t uniqueTokens,
                                  const AnchorPaddingRootToken& anchorPaddingRootToken,
                                  ECStats* escStats,
                                  std::size_t maxDocsPerInsert = write_ops::kMaxWriteBatchSize);

/**
 * Performs cleanup of the ESC entries for the encrypted field/value pair
 * whose tokens are in the provided ECOC compaction document.
 * Returns a list of the IDs of anchors to be deleted from the ESC. The length
 * of this list is capped by maxAnchorListLength.
 * Used by unit tests.
 */
enum class FLECleanupOneMode {
    kNormal,
    kPadding,
};

std::vector<PrfBlock> cleanupOneFieldValuePair(FLEQueryInterface* queryImpl,
                                               HmacContext* hmacCtx,
                                               const ECOCCompactionDocumentV2& ecocDoc,
                                               const NamespaceString& escNss,
                                               std::size_t maxAnchorListLength,
                                               ECStats* escStats,
                                               FLECleanupOneMode mode = FLECleanupOneMode::kNormal);

/**
 * Container for the _id values of ESC entries that are slated for deletion
 * at the end of a compact or cleanup operation.
 */
class FLECompactESCDeleteSet {
public:
    std::size_t size() const {
        return std::accumulate(deleteIdSets.begin(),
                               deleteIdSets.end(),
                               std::size_t{0},
                               [](const auto& sum, auto& d) { return sum + d.size(); });
    }
    bool empty() const {
        return deleteIdSets.empty();
    }

    void clear() {
        deleteIdSets.clear();
    }

    const PrfBlock& at(size_t index) const;

    std::vector<std::vector<PrfBlock>> deleteIdSets;
};

FLECompactESCDeleteSet readRandomESCNonAnchorIds(OperationContext* opCtx,
                                                 const NamespaceString& escNss,
                                                 size_t memoryLimit,
                                                 ECStats* escStats);

/**
 * Deletes from the ESC collection the non-anchor documents whose _ids
 * appear in the list deleteIds
 */
void cleanupESCNonAnchors(OperationContext* opCtx,
                          const NamespaceString& escNss,
                          const FLECompactESCDeleteSet& deleteSet,
                          size_t maxTagsPerDelete,
                          ECStats* escStats);

/**
 * Deletes from the ESC collection the anchor documents whose _ids
 * appear in the priority queue pq
 */
void cleanupESCAnchors(OperationContext* opCtx,
                       const NamespaceString& escNss,
                       FLECleanupESCDeleteQueue& pq,
                       size_t maxTagsPerDelete,
                       ECStats* escStats);

}  // namespace mongo
