/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/db/repl/tenant_migration_util.h"

#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/logical_time_validator.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/repl_server_parameters_gen.h"
#include "mongo/db/repl/wait_for_majority_service.h"
#include "mongo/util/cancelation.h"
#include "mongo/util/future_util.h"

namespace mongo {

namespace tenant_migration_util {

ExternalKeysCollectionDocument makeExternalClusterTimeKeyDoc(ServiceContext* serviceContext,
                                                             std::string rsName,
                                                             BSONObj keyDoc) {
    auto originalKeyDoc = KeysCollectionDocument::parse(IDLParserErrorContext("keyDoc"), keyDoc);

    ExternalKeysCollectionDocument externalKeyDoc(
        OID::gen(),
        originalKeyDoc.getKeyId(),
        rsName,
        serviceContext->getFastClockSource()->now() +
            Seconds{repl::tenantMigrationExternalKeysRemovalDelaySecs.load()});
    externalKeyDoc.setKeysCollectionDocumentBase(originalKeyDoc.getKeysCollectionDocumentBase());

    return externalKeyDoc;
}

ExecutorFuture<void> storeExternalClusterTimeKeyDocsAndRefreshCache(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    std::vector<ExternalKeysCollectionDocument> keyDocs,
    const CancelationToken& token) {
    auto opCtxHolder = cc().makeOperationContext();
    auto opCtx = opCtxHolder.get();
    auto nss = NamespaceString::kExternalKeysCollectionNamespace;

    for (auto& keyDoc : keyDocs) {
        AutoGetCollection collection(opCtx, nss, MODE_IX);

        writeConflictRetry(opCtx, "CloneExternalKeyDocs", nss.ns(), [&] {
            const auto filter = BSON(ExternalKeysCollectionDocument::kKeyIdFieldName
                                     << keyDoc.getKeyId()
                                     << ExternalKeysCollectionDocument::kReplicaSetNameFieldName
                                     << keyDoc.getReplicaSetName()
                                     << ExternalKeysCollectionDocument::kTTLExpiresAtFieldName
                                     << BSON("$lte" << keyDoc.getTTLExpiresAt()));

            // Remove _id since updating _id is not allowed.
            const auto updateMod = keyDoc.toBSON().removeField("_id");

            Helpers::upsert(opCtx,
                            nss.ns(),
                            filter,
                            updateMod,
                            /*fromMigrate=*/false);
        });
    }

    const auto opTime = repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();

    return WaitForMajorityService::get(opCtx->getServiceContext())
        .waitUntilMajority(opTime, CancelationToken::uncancelable())
        .thenRunOn(**executor)
        .then([] {
            auto opCtxHolder = cc().makeOperationContext();
            auto opCtx = opCtxHolder.get();

            auto validator = LogicalTimeValidator::get(opCtx);
            if (validator) {
                // Refresh the keys cache to avoid validation errors for external cluster times with
                // a keyId that matches the keyId of an internal key since the LogicalTimeValidator
                // only refreshes the cache when it cannot find a matching internal key.
                validator->refreshKeyManagerCache(opCtx);
            }
        });
}

void createRetryableWritesView(OperationContext* opCtx, Database* db) {
    writeConflictRetry(
        opCtx, "createDonorOplogView", "local.system.tenantMigration.oplogView", [&] {
            {
                // Create 'system.views' in a separate WUOW if it does not exist.
                WriteUnitOfWork wuow(opCtx);
                CollectionPtr coll = CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(
                    opCtx, NamespaceString(db->getSystemViewsName()));
                if (!coll) {
                    coll = db->createCollection(opCtx, NamespaceString(db->getSystemViewsName()));
                }
                invariant(coll);
                wuow.commit();
            }

            // First match entries with a `stmtId` so that we're filtering for retryable writes
            // oplog entries. Pass the result into the next stage of the pipeline and only project
            // the fields that a tenant migration recipient needs to refetch retryable writes oplog
            // entries: `ts`, `prevOpTime`, `preImageOpTime`, and `postImageOpTime`.
            CollectionOptions options;
            options.viewOn = NamespaceString::kRsOplogNamespace.coll().toString();
            options.pipeline = BSON_ARRAY(
                BSON("$match" << BSON("stmtId" << BSON("$exists" << true)))
                << BSON("$project" << BSON("_id"
                                           << "$ts"
                                           << "ns" << 1 << "ts" << 1 << "prevOpTime" << 1
                                           << "preImageOpTime" << 1 << "postImageOpTime" << 1)));

            WriteUnitOfWork wuow(opCtx);
            uassertStatusOK(db->createView(
                opCtx, NamespaceString("local.system.tenantMigration.oplogView"), options));
            wuow.commit();
        });
}

}  // namespace tenant_migration_util

}  // namespace mongo
