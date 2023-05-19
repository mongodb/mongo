/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/op_observer/fallback_op_observer.h"

#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/keys_collection_document_gen.h"
#include "mongo/db/logical_time_validator.h"
#include "mongo/db/op_observer/batched_write_context.h"
#include "mongo/db/op_observer/op_observer_util.h"
#include "mongo/db/read_write_concern_defaults.h"
#include "mongo/db/session/session_catalog.h"
#include "mongo/db/session/session_catalog_mongod.h"
#include "mongo/db/session/session_killer.h"
#include "mongo/db/views/util.h"
#include "mongo/db/views/view_catalog_helpers.h"
#include "mongo/util/namespace_string_util.h"

namespace mongo {

void FallbackOpObserver::onInserts(OperationContext* opCtx,
                                   const CollectionPtr& coll,
                                   std::vector<InsertStatement>::const_iterator first,
                                   std::vector<InsertStatement>::const_iterator last,
                                   std::vector<bool> fromMigrate,
                                   bool defaultFromMigrate,
                                   InsertsOpStateAccumulator* opAccumulator) {
    auto txnParticipant = TransactionParticipant::get(opCtx);
    const bool inMultiDocumentTransaction =
        txnParticipant && opCtx->writesAreReplicated() && txnParticipant.transactionIsOpen();
    if (inMultiDocumentTransaction && !opCtx->getWriteUnitOfWork()) {
        return;
    }

    const auto& nss = coll->ns();

    if (nss.isSystemDotJavascript()) {
        Scope::storedFuncMod(opCtx);
    } else if (nss.isSystemDotViews()) {
        try {
            for (auto it = first; it != last; it++) {
                view_util::validateViewDefinitionBSON(opCtx, it->doc, nss.dbName());

                uassertStatusOK(CollectionCatalog::get(opCtx)->createView(
                    opCtx,
                    NamespaceStringUtil::deserialize(nss.dbName().tenantId(),
                                                     it->doc.getStringField("_id")),
                    NamespaceStringUtil::parseNamespaceFromDoc(nss.dbName(),
                                                               it->doc.getStringField("viewOn")),
                    BSONArray{it->doc.getObjectField("pipeline")},
                    view_catalog_helpers::validatePipeline,
                    it->doc.getObjectField("collation"),
                    ViewsForDatabase::Durability::kAlreadyDurable));
            }
        } catch (const DBException&) {
            // If a previous operation left the view catalog in an invalid state, our inserts can
            // fail even if all the definitions are valid. Reloading may help us reset the state.
            CollectionCatalog::get(opCtx)->reloadViews(opCtx, nss.dbName());
        }
    } else if (nss == NamespaceString::kSessionTransactionsTableNamespace) {
        if (opAccumulator) {
            auto& opTimeList = opAccumulator->opTimes;
            if (!opTimeList.empty() && !opTimeList.back().isNull()) {
                for (auto it = first; it != last; it++) {
                    auto mongoDSessionCatalog = MongoDSessionCatalog::get(opCtx);
                    mongoDSessionCatalog->observeDirectWriteToConfigTransactions(opCtx, it->doc);
                }
            }
        }
    } else if (nss == NamespaceString::kConfigSettingsNamespace) {
        for (auto it = first; it != last; it++) {
            ReadWriteConcernDefaults::get(opCtx).observeDirectWriteToConfigSettings(
                opCtx, it->doc["_id"], it->doc);
        }
    } else if (nss == NamespaceString::kExternalKeysCollectionNamespace) {
        for (auto it = first; it != last; it++) {
            auto externalKey =
                ExternalKeysCollectionDocument::parse(IDLParserContext("externalKey"), it->doc);
            opCtx->recoveryUnit()->onCommit(
                [this, externalKey = std::move(externalKey)](OperationContext* opCtx,
                                                             boost::optional<Timestamp>) mutable {
                    auto validator = LogicalTimeValidator::get(opCtx);
                    if (validator) {
                        validator->cacheExternalKey(externalKey);
                    }
                });
        }
    }
}

void FallbackOpObserver::onUpdate(OperationContext* opCtx,
                                  const OplogUpdateEntryArgs& args,
                                  OpStateAccumulator* opAccumulator) {
    if (args.updateArgs->update.isEmpty()) {
        return;
    }

    const auto& nss = args.coll->ns();

    if (nss.isSystemDotJavascript()) {
        Scope::storedFuncMod(opCtx);
    } else if (nss.isSystemDotViews()) {
        CollectionCatalog::get(opCtx)->reloadViews(opCtx, nss.dbName());
    } else if (nss == NamespaceString::kSessionTransactionsTableNamespace &&
               !opAccumulator->opTime.writeOpTime.isNull()) {
        auto mongoDSessionCatalog = MongoDSessionCatalog::get(opCtx);
        mongoDSessionCatalog->observeDirectWriteToConfigTransactions(opCtx,
                                                                     args.updateArgs->updatedDoc);
    } else if (nss == NamespaceString::kConfigSettingsNamespace) {
        ReadWriteConcernDefaults::get(opCtx).observeDirectWriteToConfigSettings(
            opCtx, args.updateArgs->updatedDoc["_id"], args.updateArgs->updatedDoc);
    }
}

void FallbackOpObserver::onDelete(OperationContext* opCtx,
                                  const CollectionPtr& coll,
                                  StmtId stmtId,
                                  const OplogDeleteEntryArgs& args,
                                  OpStateAccumulator* opAccumulator) {
    const auto& nss = coll->ns();
    const bool inBatchedWrite = BatchedWriteContext::get(opCtx).writesAreBatched();

    auto optDocKey = repl::documentKeyDecoration(opCtx);
    invariant(optDocKey, nss.toStringForErrorMsg());
    auto& documentKey = optDocKey.value();

    if (nss.isSystemDotJavascript()) {
        Scope::storedFuncMod(opCtx);
    } else if (nss.isSystemDotViews()) {
        CollectionCatalog::get(opCtx)->reloadViews(opCtx, nss.dbName());
    } else if (nss == NamespaceString::kSessionTransactionsTableNamespace &&
               (inBatchedWrite || !opAccumulator->opTime.writeOpTime.isNull())) {
        auto mongoDSessionCatalog = MongoDSessionCatalog::get(opCtx);
        mongoDSessionCatalog->observeDirectWriteToConfigTransactions(opCtx, documentKey.getId());
    } else if (nss == NamespaceString::kConfigSettingsNamespace) {
        ReadWriteConcernDefaults::get(opCtx).observeDirectWriteToConfigSettings(
            opCtx, documentKey.getId().firstElement(), boost::none);
    }
}

void FallbackOpObserver::onDropDatabase(OperationContext* opCtx, const DatabaseName& dbName) {
    if (dbName.db() == NamespaceString::kSessionTransactionsTableNamespace.db()) {
        auto mongoDSessionCatalog = MongoDSessionCatalog::get(opCtx);
        mongoDSessionCatalog->invalidateAllSessions(opCtx);
    }
}

repl::OpTime FallbackOpObserver::onDropCollection(OperationContext* opCtx,
                                                  const NamespaceString& collectionName,
                                                  const UUID& uuid,
                                                  std::uint64_t numRecords,
                                                  CollectionDropType dropType,
                                                  bool markFromMigrate) {
    if (collectionName.isSystemDotJavascript()) {
        Scope::storedFuncMod(opCtx);
    } else if (collectionName.isSystemDotViews()) {
        CollectionCatalog::get(opCtx)->clearViews(opCtx, collectionName.dbName());
    } else if (collectionName == NamespaceString::kSessionTransactionsTableNamespace) {
        // Disallow this drop if there are currently prepared transactions.
        const auto sessionCatalog = SessionCatalog::get(opCtx);
        SessionKiller::Matcher matcherAllSessions(
            KillAllSessionsByPatternSet{makeKillAllSessionsByPattern(opCtx)});
        bool noPreparedTxns = true;
        sessionCatalog->scanSessions(matcherAllSessions, [&](const ObservableSession& session) {
            auto txnParticipant = TransactionParticipant::get(session);
            if (txnParticipant.transactionIsPrepared()) {
                noPreparedTxns = false;
            }
        });
        uassert(4852500,
                "Unable to drop transactions table (config.transactions) while prepared "
                "transactions are present.",
                noPreparedTxns);

        auto mongoDSessionCatalog = MongoDSessionCatalog::get(opCtx);
        mongoDSessionCatalog->invalidateAllSessions(opCtx);
    } else if (collectionName == NamespaceString::kConfigSettingsNamespace) {
        ReadWriteConcernDefaults::get(opCtx).invalidate();
    }

    return {};
}

}  // namespace mongo
