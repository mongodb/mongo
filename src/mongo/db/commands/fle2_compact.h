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
    NamespaceString eccNss;
    NamespaceString ecocNss;
    NamespaceString ecocRenameNss;
};

/**
 * Validate a compact request has the right encryption tokens.
 */
void validateCompactRequest(const CompactStructuredEncryptionData& request, const Collection& edc);

CompactStats processFLECompact(OperationContext* opCtx,
                               const CompactStructuredEncryptionData& request,
                               GetTxnCallback getTxn,
                               const EncryptedStateCollectionsNamespaces& namespaces);

/**
 * Get all unique documents in the ECOC collection in their decrypted form.
 *
 * Used by unit tests.
 */
stdx::unordered_set<ECOCCompactionDocument> getUniqueCompactionDocuments(
    FLEQueryInterface* queryImpl,
    const CompactStructuredEncryptionData& request,
    const NamespaceString& ecocNss,
    ECOCStats* ecocStats);

/**
 * Performs compaction of the ESC and ECC entries for the encrypted field/value pair
 * whose tokens are in the provided ECOC compaction document.
 *
 * Used by unit tests.
 */
void compactOneFieldValuePair(FLEQueryInterface* queryImpl,
                              const ECOCCompactionDocument& ecocDoc,
                              const EncryptedStateCollectionsNamespaces& namespaces,
                              ECStats* escStats,
                              ECStats* eccStats);
}  // namespace mongo
