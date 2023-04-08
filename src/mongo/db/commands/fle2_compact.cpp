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


#include "mongo/platform/basic.h"

#include "mongo/db/commands/fle2_compact.h"

#include <memory>

#include "mongo/bson/util/bson_extract.h"
#include "mongo/crypto/encryption_fields_gen.h"
#include "mongo/crypto/fle_stats.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/commands/fle2_compact_gen.h"
#include "mongo/db/commands/server_status.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/mutex.h"
#include "mongo/util/fail_point.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kWrite


MONGO_FAIL_POINT_DEFINE(fleCompactFailBeforeECOCRead);
MONGO_FAIL_POINT_DEFINE(fleCompactHangBeforeESCAnchorInsert);


namespace mongo {
namespace {

constexpr auto kId = "_id"_sd;
constexpr auto kValue = "value"_sd;

/**
 * Wrapper class around the IDL stats types that enables easier
 * addition to the statistics counters.
 * Wrapped object must outlive this object.
 */
template <typename IDLType>
class CompactStatsCounter {
public:
    CompactStatsCounter(IDLType* wrappedType) : _stats(wrappedType) {}
    void addReads(std::int64_t n) {
        _stats->setRead(_stats->getRead() + n);
    }
    void addDeletes(std::int64_t n) {
        _stats->setDeleted(_stats->getDeleted() + n);
    }
    void addInserts(std::int64_t n) {
        _stats->setInserted(_stats->getInserted() + n);
    }
    void addUpdates(std::int64_t n) {
        _stats->setUpdated(_stats->getUpdated() + n);
    }
    void add(const IDLType& other) {
        addReads(other.getRead());
        addDeletes(other.getDeleted());
        addInserts(other.getInserted());
        addUpdates(other.getUpdated());
    }

private:
    IDLType* _stats;
};

/**
 * ECOCStats specializations of these functions are no-ops
 * since ECOCStats does not have insert and update counters
 */
template <>
void CompactStatsCounter<ECOCStats>::addInserts(std::int64_t n) {}
template <>
void CompactStatsCounter<ECOCStats>::addUpdates(std::int64_t n) {}
template <>
void CompactStatsCounter<ECOCStats>::add(const ECOCStats& other) {
    addReads(other.getRead());
    addDeletes(other.getDeleted());
}

/**
 * Implementation of FLEStateCollectionReader for txn_api::TransactionClient
 */
template <typename StatsType>
class TxnCollectionReaderForCompact : public FLEStateCollectionReader {
public:
    TxnCollectionReaderForCompact(FLEQueryInterface* queryImpl,
                                  const NamespaceString& nss,
                                  StatsType* stats)
        : _queryImpl(queryImpl), _nss(nss), _stats(stats) {}

    uint64_t getDocumentCount() const override {
        return _queryImpl->countDocuments(_nss);
    }

    BSONObj getById(PrfBlock block) const override {
        auto doc = BSON("v" << BSONBinData(block.data(), block.size(), BinDataGeneral));
        BSONElement element = doc.firstElement();
        auto result = _queryImpl->getById(_nss, element);
        _stats.addReads(1);
        return result;
    }

private:
    FLEQueryInterface* _queryImpl;
    const NamespaceString& _nss;
    mutable CompactStatsCounter<StatsType> _stats;
};
}  // namespace


StatusWith<EncryptedStateCollectionsNamespaces>
EncryptedStateCollectionsNamespaces::createFromDataCollection(const Collection& edc) {
    if (!edc.getCollectionOptions().encryptedFieldConfig) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "Encrypted data collection " << edc.ns()
                                    << " is missing encrypted fields metadata");
    }

    auto& cfg = *(edc.getCollectionOptions().encryptedFieldConfig);
    auto db = edc.ns().db();
    StringData missingColl;
    EncryptedStateCollectionsNamespaces namespaces;

    auto f = [&missingColl](StringData coll) {
        missingColl = coll;
        return StringData();
    };

    namespaces.edcNss = edc.ns();
    namespaces.escNss =
        NamespaceString(db, cfg.getEscCollection().value_or_eval([&f]() { return f("state"_sd); }));

    namespaces.ecocNss = NamespaceString(
        db, cfg.getEcocCollection().value_or_eval([&f]() { return f("compaction"_sd); }));

    if (!missingColl.empty()) {
        return Status(ErrorCodes::BadValue,
                      str::stream()
                          << "Encrypted data collection " << edc.ns()
                          << " is missing the name of its " << missingColl << " collection");
    }

    namespaces.ecocRenameNss =
        NamespaceString(db, namespaces.ecocNss.coll().toString().append(".compact"));
    return namespaces;
}

/**
 * Parses the compaction tokens from the compact request, and
 * for each one, retrieves the unique entries in the ECOC collection
 * that have been encrypted with that token. All entries are returned
 * in a set in their decrypted form.
 */
stdx::unordered_set<ECOCCompactionDocumentV2> getUniqueCompactionDocumentsV2(
    FLEQueryInterface* queryImpl,
    const CompactStructuredEncryptionData& request,
    const NamespaceString& ecocNss,
    ECOCStats* ecocStats) {

    CompactStatsCounter<ECOCStats> stats(ecocStats);

    // Initialize a set 'C' and for each compaction token, find all entries
    // in ECOC with matching field name. Decrypt entries and add to set 'C'.
    stdx::unordered_set<ECOCCompactionDocumentV2> c;
    auto compactionTokens = CompactionHelpers::parseCompactionTokens(request.getCompactionTokens());

    for (auto& compactionToken : compactionTokens) {
        auto docs = queryImpl->findDocuments(
            ecocNss, BSON(EcocDocument::kFieldNameFieldName << compactionToken.fieldPathName));
        stats.addReads(docs.size());

        for (auto& doc : docs) {
            auto ecocDoc = ECOCCollection::parseAndDecryptV2(doc, compactionToken.token);
            c.insert(std::move(ecocDoc));
        }
    }
    return c;
}

void compactOneFieldValuePairV2(FLEQueryInterface* queryImpl,
                                const ECOCCompactionDocumentV2& ecocDoc,
                                const NamespaceString& escNss,
                                ECStats* escStats) {
    auto escTagToken = FLETwiceDerivedTokenGenerator::generateESCTwiceDerivedTagToken(ecocDoc.esc);
    auto escValueToken =
        FLETwiceDerivedTokenGenerator::generateESCTwiceDerivedValueToken(ecocDoc.esc);

    CompactStatsCounter<ECStats> stats(escStats);
    TxnCollectionReaderForCompact reader(queryImpl, escNss, escStats);

    auto positions = ESCCollection::emuBinaryV2(reader, escTagToken, escValueToken);

    // Handle case where cpos is none. This means that no new non-anchors have been inserted
    // since since the last compact/cleanup.
    // This could happen if a previous compact inserted an anchor, but the temp ECOC drop
    // was interrupted. On restart, the compaction will run emuBinaryV2 again, but since the
    // anchor was already inserted for this value, it may return null cpos if there have been no
    // new insertions for that value since the first compact attempt.
    if (!positions.cpos.has_value()) {
        // No new non-anchors since the last compact/cleanup.
        // There must be at least one anchor.
        uassert(7293602,
                "An ESC anchor document is expected but none is found",
                !positions.apos.has_value() || positions.apos.value() > 0);
        // the anchor with the latest cpos already exists so no more work needed
        return;
    }

    uint64_t nextAnchorPos = 0;

    if (!positions.apos.has_value()) {
        auto r_esc = reader.getById(ESCCollection::generateNullAnchorId(escTagToken));

        uassert(7293601, "ESC null anchor document not found", !r_esc.isEmpty());

        auto nullAnchorDoc =
            uassertStatusOK(ESCCollection::decryptAnchorDocument(escValueToken, r_esc));
        nextAnchorPos = nullAnchorDoc.position + 1;
    } else {
        nextAnchorPos = positions.apos.value() + 1;
    }

    auto anchorDoc = ESCCollection::generateAnchorDocument(
        escTagToken, escValueToken, nextAnchorPos, positions.cpos.value());
    StmtId stmtId = kUninitializedStmtId;

    if (MONGO_unlikely(fleCompactHangBeforeESCAnchorInsert.shouldFail())) {
        LOGV2(7293606, "Hanging due to fleCompactHangBeforeESCAnchorInsert fail point");
        fleCompactHangBeforeESCAnchorInsert.pauseWhileSet();
    }

    auto insertReply =
        uassertStatusOK(queryImpl->insertDocuments(escNss, {anchorDoc}, &stmtId, true));
    checkWriteErrors(insertReply);
    stats.addInserts(1);
}

void processFLECompactV2(OperationContext* opCtx,
                         const CompactStructuredEncryptionData& request,
                         GetTxnCallback getTxn,
                         const EncryptedStateCollectionsNamespaces& namespaces,
                         ECStats* escStats,
                         ECOCStats* ecocStats) {
    auto innerEcocStats = std::make_shared<ECOCStats>();
    auto innerEscStats = std::make_shared<ECStats>();

    /* uniqueEcocEntries corresponds to the set 'C_f' in OST-1 */
    auto uniqueEcocEntries = std::make_shared<stdx::unordered_set<ECOCCompactionDocumentV2>>();

    if (MONGO_unlikely(fleCompactFailBeforeECOCRead.shouldFail())) {
        uasserted(7293605, "Failed compact due to fleCompactFailBeforeECOCRead fail point");
    }

    // Read the ECOC documents in a transaction
    {
        std::shared_ptr<txn_api::SyncTransactionWithRetries> trun = getTxn(opCtx);

        // The function that handles the transaction may outlive this function so we need to use
        // shared_ptrs
        auto argsBlock = std::make_tuple(request, namespaces.ecocRenameNss);
        auto sharedBlock = std::make_shared<decltype(argsBlock)>(argsBlock);

        auto swResult = trun->runNoThrow(
            opCtx,
            [sharedBlock, uniqueEcocEntries, innerEcocStats](
                const txn_api::TransactionClient& txnClient, ExecutorPtr txnExec) {
                FLEQueryInterfaceImpl queryImpl(txnClient, getGlobalServiceContext());

                auto [request2, ecocRenameNss] = *sharedBlock.get();

                *uniqueEcocEntries = getUniqueCompactionDocumentsV2(
                    &queryImpl, request2, ecocRenameNss, innerEcocStats.get());

                return SemiFuture<void>::makeReady();
            });

        uassertStatusOK(swResult);
        uassertStatusOK(swResult.getValue().getEffectiveStatus());
    }

    // Each entry in 'C_f' represents a unique field/value pair. For each field/value pair,
    // compact the ESC entries for that field/value pair in one transaction.
    for (auto& ecocDoc : *uniqueEcocEntries) {
        // start a new transaction
        std::shared_ptr<txn_api::SyncTransactionWithRetries> trun = getTxn(opCtx);

        // The function that handles the transaction may outlive this function so we need to use
        // shared_ptrs
        auto argsBlock = std::make_tuple(ecocDoc, namespaces.escNss);
        auto sharedBlock = std::make_shared<decltype(argsBlock)>(argsBlock);

        auto swResult = trun->runNoThrow(
            opCtx,
            [sharedBlock, innerEscStats](const txn_api::TransactionClient& txnClient,
                                         ExecutorPtr txnExec) {
                FLEQueryInterfaceImpl queryImpl(txnClient, getGlobalServiceContext());

                auto [ecocDoc2, escNss] = *sharedBlock.get();

                compactOneFieldValuePairV2(&queryImpl, ecocDoc2, escNss, innerEscStats.get());

                return SemiFuture<void>::makeReady();
            });

        uassertStatusOK(swResult);
        uassertStatusOK(swResult.getValue().getEffectiveStatus());
    }

    // Update stats
    if (escStats) {
        CompactStatsCounter<ECStats> ctr(escStats);
        ctr.add(*innerEscStats);
    }
    if (ecocStats) {
        CompactStatsCounter<ECOCStats> ctr(ecocStats);
        ctr.add(*innerEcocStats);
    }
}

void validateCompactRequest(const CompactStructuredEncryptionData& request, const Collection& edc) {
    uassert(6346807,
            "Target namespace is not an encrypted collection",
            edc.getCollectionOptions().encryptedFieldConfig);

    // Validate the request contains a compaction token for each encrypted field
    const auto& efc = edc.getCollectionOptions().encryptedFieldConfig.value();
    CompactionHelpers::validateCompactionTokens(efc, request.getCompactionTokens());
}

const PrfBlock& FLECompactESCDeleteSet::at(size_t index) const {
    if (index >= size()) {
        throw std::out_of_range("out of range");
    }
    for (auto& deleteSet : this->deleteIdSets) {
        if (index < deleteSet.size()) {
            return deleteSet.at(index);
        }
        index -= deleteSet.size();
    }
    MONGO_UNREACHABLE;
}

FLECompactESCDeleteSet readRandomESCNonAnchorIds(OperationContext* opCtx,
                                                 const NamespaceString& escNss,
                                                 size_t memoryLimit,
                                                 ECStats* escStats) {
    FLECompactESCDeleteSet deleteSet;

    size_t idLimit = memoryLimit / sizeof(PrfBlock);
    if (0 == idLimit) {
        return deleteSet;
    }

    DBDirectClient client(opCtx);
    AggregateCommandRequest aggCmd{escNss};

    // Build an agg pipeline for fetching a set of random ESC "non-anchor" entries.
    // Note: "Non-anchors" are entries that don't have a "value" field.
    // pipeline: [ {$match: {value: {$exists: false}}}, {$sample: {size: idLimit}} ]
    {
        std::vector<BSONObj> pipeline;
        pipeline.emplace_back(BSON("$match" << BSON(kValue << BSON("$exists" << false))));
        pipeline.emplace_back(BSON("$sample" << BSON("size" << static_cast<int64_t>(idLimit))));
        aggCmd.setPipeline(std::move(pipeline));
    }

    auto swCursor = DBClientCursor::fromAggregationRequest(&client, aggCmd, false, false);
    uassertStatusOK(swCursor.getStatus());
    auto cursor = std::move(swCursor.getValue());

    uassert(7293607, "Got an invalid cursor while reading the Queryable Encryption ESC", cursor);

    while (cursor->more()) {
        auto& deleteIds = deleteSet.deleteIdSets.emplace_back();
        deleteIds.reserve(cursor->objsLeftInBatch());

        do {
            const auto doc = cursor->nextSafe();
            BSONElement id;
            auto status = bsonExtractTypedField(doc, kId, BinData, &id);
            uassertStatusOK(status);

            uassert(7293604,
                    "Found a document in ESC with _id of incorrect BinDataType",
                    id.binDataType() == BinDataType::BinDataGeneral);
            deleteIds.emplace_back(PrfBlockfromCDR(binDataToCDR(id)));
        } while (cursor->moreInCurrentBatch());
    }

    if (escStats) {
        CompactStatsCounter<ECStats> stats(escStats);
        stats.addReads(deleteSet.size());
    }

    return deleteSet;
}

void cleanupESCNonAnchors(OperationContext* opCtx,
                          const NamespaceString& escNss,
                          const FLECompactESCDeleteSet& deleteSet,
                          size_t maxTagsPerDelete,
                          ECStats* escStats) {
    uassert(7293611,
            "Max number of ESC entries to delete per request cannot be zero",
            maxTagsPerDelete > 0);
    if (deleteSet.empty()) {
        LOGV2_DEBUG(7293608,
                    1,
                    "Queryable Encryption compaction has nothing to delete from ESC",
                    "namespace"_attr = escNss);
        return;
    }

    DBDirectClient client(opCtx);
    std::int64_t deleted = 0;

    for (size_t idIndex = 0; idIndex < deleteSet.size();) {
        write_ops::DeleteCommandRequest deleteRequest(escNss,
                                                      std::vector<write_ops::DeleteOpEntry>{});
        auto& opEntry = deleteRequest.getDeletes().emplace_back();
        opEntry.setMulti(true);

        BSONObjBuilder queryBuilder;
        {
            BSONObjBuilder idBuilder(queryBuilder.subobjStart(kId));
            BSONArrayBuilder array = idBuilder.subarrayStart("$in");
            size_t tagLimit = std::min(deleteSet.size() - idIndex, maxTagsPerDelete);

            for (size_t tags = 0; tags < tagLimit; tags++) {
                const auto& id = deleteSet.at(idIndex++);
                array.appendBinData(id.size(), BinDataGeneral, id.data());
            }
        }
        opEntry.setQ(queryBuilder.obj());

        auto reply = client.remove(deleteRequest);
        if (reply.getWriteCommandReplyBase().getWriteErrors()) {
            LOGV2_WARNING(7293609,
                          "Queryable Encryption compaction encountered write errors",
                          "namespace"_attr = escNss,
                          "reply"_attr = reply);
        }
        deleted += reply.getN();
    }

    if (escStats) {
        CompactStatsCounter<ECStats> stats(escStats);
        stats.addDeletes(deleted);
    }
}

}  // namespace mongo
