/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#pragma once

#include "mongo/base/status_with.h"
#include "mongo/db/commands/fle2_compact_gen.h"
#include "mongo/db/fle_crud.h"

namespace mongo {

struct EncryptedStateCollectionsNamespaces {

    static StatusWith<EncryptedStateCollectionsNamespaces> createFromDataCollection(
        const Collection& edc);

    NamespaceString edcNss;
    NamespaceString escNss;
    NamespaceString ecocNss;
    NamespaceString ecocRenameNss;
};

/**
 * Validate a compact request has the right encryption tokens.
 */
void validateCompactRequest(const CompactStructuredEncryptionData& request, const Collection& edc);

void processFLECompactV2(OperationContext* opCtx,
                         const CompactStructuredEncryptionData& request,
                         GetTxnCallback getTxn,
                         const EncryptedStateCollectionsNamespaces& namespaces,
                         ECStats* escStats,
                         ECOCStats* ecocStats);

/**
 * Get all unique documents in the ECOC collection in their decrypted form.
 *
 * Used by unit tests.
 */
stdx::unordered_set<ECOCCompactionDocumentV2> getUniqueCompactionDocumentsV2(
    FLEQueryInterface* queryImpl,
    const CompactStructuredEncryptionData& request,
    const NamespaceString& ecocNss,
    ECOCStats* ecocStats);


/**
 * Performs compaction of the ESC entries for the encrypted field/value pair
 * whose tokens are in the provided ECOC compaction document.
 *
 * Used by unit tests.
 */
void compactOneFieldValuePairV2(FLEQueryInterface* queryImpl,
                                const ECOCCompactionDocumentV2& ecocDoc,
                                const NamespaceString& escNss,
                                ECStats* escStats);

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
 * Deletes from the ESC collection the non-anchor documents whose _id
 * appears in the list deleteIds
 */
void cleanupESCNonAnchors(OperationContext* opCtx,
                          const NamespaceString& escNss,
                          const FLECompactESCDeleteSet& deleteSet,
                          size_t maxTagsPerDelete,
                          ECStats* escStats);

}  // namespace mongo
