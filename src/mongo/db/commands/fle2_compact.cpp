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

#include "mongo/crypto/encryption_fields_gen.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/commands/fle2_compact_gen.h"
#include "mongo/db/commands/server_status.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/mutex.h"
#include "mongo/util/fail_point.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kWrite


MONGO_FAIL_POINT_DEFINE(fleCompactFailBeforeECOCRead);
MONGO_FAIL_POINT_DEFINE(fleCompactHangBeforeESCPlaceholderInsert);
MONGO_FAIL_POINT_DEFINE(fleCompactHangAfterESCPlaceholderInsert);
MONGO_FAIL_POINT_DEFINE(fleCompactHangBeforeECCPlaceholderInsert);
MONGO_FAIL_POINT_DEFINE(fleCompactHangAfterECCPlaceholderInsert);

namespace mongo {
namespace {
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

/**
 * Implementation of FLEStateCollectionReader for txn_api::TransactionClient
 */
template <typename StatsType>
class TxnCollectionReader : public FLEStateCollectionReader {
public:
    TxnCollectionReader(FLEQueryInterface* queryImpl, const NamespaceString& nss, StatsType* stats)
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

/**
 * Deletes an entry at the given position from FLECollection, using
 * the TagToken to generate the _id value for the delete query.
 */
template <typename FLECollection, typename TagToken>
void deleteDocumentByPos(FLEQueryInterface* queryImpl,
                         const NamespaceString& nss,
                         uint64_t pos,
                         const TagToken& tagToken,
                         ECStats* stats) {
    CompactStatsCounter<ECStats> statsCtr(stats);

    write_ops::DeleteOpEntry deleteEntry;
    auto block = FLECollection::generateId(tagToken, pos);
    deleteEntry.setMulti(false);
    deleteEntry.setQ(BSON("_id" << BSONBinData(block.data(), block.size(), BinDataGeneral)));
    write_ops::DeleteCommandRequest deleteRequest(nss, {std::move(deleteEntry)});
    auto [deleteReply, deletedDoc] =
        queryImpl->deleteWithPreimage(nss, EncryptionInformation(BSONObj()), deleteRequest);

    if (deletedDoc.isEmpty()) {
        // nothing was deleted
        return;
    }
    checkWriteErrors(deleteReply);
    statsCtr.addDeletes(1);
}

/**
 * Inserts or updates a null document in FLECollection.
 * The newNullDoc must contain the _id of the null document to update.
 */
void upsertNullDocument(FLEQueryInterface* queryImpl,
                        bool hasNullDoc,
                        BSONObj newNullDoc,
                        const NamespaceString& nss,
                        ECStats* stats) {
    CompactStatsCounter<ECStats> statsCtr(stats);
    if (hasNullDoc) {
        // update the null doc with a replacement modification
        write_ops::UpdateOpEntry updateEntry;
        updateEntry.setMulti(false);
        updateEntry.setUpsert(false);
        updateEntry.setQ(newNullDoc.getField("_id").wrap());
        updateEntry.setU(mongo::write_ops::UpdateModification(
            newNullDoc, write_ops::UpdateModification::ReplacementTag{}));
        write_ops::UpdateCommandRequest updateRequest(nss, {std::move(updateEntry)});
        auto [reply, originalDoc] =
            queryImpl->updateWithPreimage(nss, EncryptionInformation(BSONObj()), updateRequest);
        checkWriteErrors(reply);
        if (!originalDoc.isEmpty()) {
            statsCtr.addUpdates(1);
        }
    } else {
        // insert the null doc; translate duplicate key error to a FLE contention error
        StmtId stmtId = kUninitializedStmtId;
        auto reply = uassertStatusOK(queryImpl->insertDocument(nss, newNullDoc, &stmtId, true));
        checkWriteErrors(reply);
        statsCtr.addInserts(1);
    }
}

/**
 * Deletes a document at the specified position from the ESC
 */
void deleteESCDocument(FLEQueryInterface* queryImpl,
                       const NamespaceString& nss,
                       uint64_t pos,
                       const ESCTwiceDerivedTagToken& tagToken,
                       ECStats* escStats) {
    deleteDocumentByPos<ESCCollection, ESCTwiceDerivedTagToken>(
        queryImpl, nss, pos, tagToken, escStats);
}

/**
 * Deletes a document at the specified position from the ECC
 */
void deleteECCDocument(FLEQueryInterface* queryImpl,
                       const NamespaceString& nss,
                       uint64_t pos,
                       const ECCTwiceDerivedTagToken& tagToken,
                       ECStats* eccStats) {
    deleteDocumentByPos<ECCCollection, ECCTwiceDerivedTagToken>(
        queryImpl, nss, pos, tagToken, eccStats);
}

/**
 * Result of preparing the ESC collection for a single field/value pair
 * before compaction.
 */
struct ESCPreCompactState {
    // total insertions of this field/value pair into EDC
    uint64_t count{0};
    // position of the lowest entry
    uint64_t ipos{0};
    // position of the highest entry
    uint64_t pos{0};
};

/**
 * Finds the upper and lower bound positions, and the current counter
 * value from the ESC collection for the given twice-derived tokens,
 * and inserts the compaction placeholder document.
 */
ESCPreCompactState prepareESCForCompaction(FLEQueryInterface* queryImpl,
                                           const NamespaceString& nssEsc,
                                           const ESCTwiceDerivedTagToken& tagToken,
                                           const ESCTwiceDerivedValueToken& valueToken,
                                           ECStats* escStats) {
    CompactStatsCounter<ECStats> stats(escStats);

    TxnCollectionReader reader(queryImpl, nssEsc, escStats);

    // get the upper bound index 'pos' using binary search
    // get the lower bound index 'ipos' from the null doc, if it exists, otherwise 1
    ESCPreCompactState state;

    auto alpha = ESCCollection::emuBinary(reader, tagToken, valueToken);
    if (alpha.has_value() && alpha.value() == 0) {
        // No null doc & no entries found for this field/value pair so nothing to compact.
        // This can happen if the tag and value tokens were derived from a bogus ECOC
        // document, or from an ECOC document decrypted with bogus compaction tokens.
        // Skip inserting the compaction placeholder.
        return state;
    } else if (!alpha.has_value()) {
        // only the null doc exists
        auto block = ESCCollection::generateId(tagToken, boost::none);
        auto r_esc = reader.getById(block);
        uassert(6346802, "ESC null document not found", !r_esc.isEmpty());

        auto nullDoc = uassertStatusOK(ESCCollection::decryptNullDocument(valueToken, r_esc));

        // +2 to skip over index of placeholder doc from previous compaction
        state.pos = nullDoc.position + 2;
        state.ipos = state.pos;
        state.count = nullDoc.count;
    } else {
        // one or more entries exist for this field/value pair
        auto block = ESCCollection::generateId(tagToken, alpha);
        auto r_esc = reader.getById(block);
        uassert(6346803, "ESC document not found", !r_esc.isEmpty());

        auto escDoc = uassertStatusOK(ESCCollection::decryptDocument(valueToken, r_esc));

        state.pos = alpha.value() + 1;
        state.count = escDoc.count;

        // null doc may or may not yet exist
        block = ESCCollection::generateId(tagToken, boost::none);
        r_esc = reader.getById(block);
        if (r_esc.isEmpty()) {
            state.ipos = 1;
        } else {
            auto nullDoc = uassertStatusOK(ESCCollection::decryptNullDocument(valueToken, r_esc));
            state.ipos = nullDoc.position + 2;
        }
    }

    uassert(6346804, "Invalid position range for ESC compact", state.ipos <= state.pos);
    uassert(6346805, "Invalid counter value for ESC compact", state.count > 0);

    // Insert a placeholder at the next ESC position; this is deleted later in compact.
    // This serves to trigger a write conflict if another write transaction is
    // committed before the current compact transaction commits
    if (MONGO_unlikely(fleCompactHangBeforeESCPlaceholderInsert.shouldFail())) {
        LOGV2(6548301, "Hanging due to fleCompactHangBeforeESCPlaceholderInsert fail point");
        fleCompactHangBeforeESCPlaceholderInsert.pauseWhileSet();
    }

    auto placeholder = ESCCollection::generateCompactionPlaceholderDocument(
        tagToken, valueToken, state.pos, state.count);
    StmtId stmtId = kUninitializedStmtId;
    auto insertReply =
        uassertStatusOK(queryImpl->insertDocument(nssEsc, placeholder, &stmtId, true));
    checkWriteErrors(insertReply);
    stats.addInserts(1);

    if (MONGO_unlikely(fleCompactHangAfterESCPlaceholderInsert.shouldFail())) {
        LOGV2(6548302, "Hanging due to fleCompactHangAfterESCPlaceholderInsert fail point");
        fleCompactHangAfterESCPlaceholderInsert.pauseWhileSet();
    }
    return state;
}

/**
 * Result of preparing the ECC collection for a single field/value pair
 * before compaction.
 */
struct ECCPreCompactState {
    // total deletions of this field/value pair from EDC
    uint64_t count{0};
    // position of the lowest entry
    uint64_t ipos{0};
    // position of the highest entry
    uint64_t pos{0};
    // result of merging all ECC entries for this field/value pair
    std::vector<ECCDocument> g_prime;
    // whether the merge reduced the number of ECC entries
    bool merged{false};
};

ECCPreCompactState prepareECCForCompaction(FLEQueryInterface* queryImpl,
                                           const NamespaceString& nssEcc,
                                           const ECCTwiceDerivedTagToken& tagToken,
                                           const ECCTwiceDerivedValueToken& valueToken,
                                           ECStats* eccStats) {
    CompactStatsCounter<ECStats> stats(eccStats);

    TxnCollectionReader reader(queryImpl, nssEcc, eccStats);

    ECCPreCompactState state;
    bool flag = true;
    std::vector<ECCDocument> g;

    // find the null doc
    auto block = ECCCollection::generateId(tagToken, boost::none);
    auto r_ecc = reader.getById(block);
    if (r_ecc.isEmpty()) {
        state.pos = 1;
    } else {
        auto nullDoc = uassertStatusOK(ECCCollection::decryptNullDocument(valueToken, r_ecc));
        state.pos = nullDoc.position + 2;
    }

    // get all documents starting from ipos; set pos to one after position of last document found
    state.ipos = state.pos;
    while (flag) {
        block = ECCCollection::generateId(tagToken, state.pos);
        r_ecc = reader.getById(block);
        if (!r_ecc.isEmpty()) {
            auto doc = uassertStatusOK(ECCCollection::decryptDocument(valueToken, r_ecc));
            g.push_back(std::move(doc));
            state.pos += 1;
        } else {
            flag = false;
        }
    }

    if (g.empty()) {
        // if there are no entries, there must not be a null doc and ipos must be 1
        uassert(6346901, "Found ECC null doc, but no ECC entries", state.ipos == 1);

        // no null doc & no entries found, so nothing to compact
        state.pos = 0;
        state.ipos = 0;
        state.count = 0;
        return state;
    }

    // merge 'g'
    state.g_prime = CompactionHelpers::mergeECCDocuments(g);
    dassert(std::is_sorted(g.begin(), g.end()));
    dassert(std::is_sorted(state.g_prime.begin(), state.g_prime.end()));
    state.merged = (state.g_prime != g);
    state.count = CompactionHelpers::countDeleted(state.g_prime);

    if (state.merged) {
        // Insert a placeholder at the next ECC position; this is deleted later in compact.
        // This serves to trigger a write conflict if another write transaction is
        // committed before the current compact transaction commits
        auto placeholder =
            ECCCollection::generateCompactionDocument(tagToken, valueToken, state.pos);
        StmtId stmtId = kUninitializedStmtId;

        if (MONGO_unlikely(fleCompactHangBeforeECCPlaceholderInsert.shouldFail())) {
            LOGV2(6548303, "Hanging due to fleCompactHangBeforeECCPlaceholderInsert fail point");
            fleCompactHangBeforeECCPlaceholderInsert.pauseWhileSet();
        }
        auto insertReply =
            uassertStatusOK(queryImpl->insertDocument(nssEcc, placeholder, &stmtId, true));
        checkWriteErrors(insertReply);
        stats.addInserts(1);

        if (MONGO_unlikely(fleCompactHangAfterECCPlaceholderInsert.shouldFail())) {
            LOGV2(6548304, "Hanging due to fleCompactHangAfterECCPlaceholderInsert fail point");
            fleCompactHangAfterECCPlaceholderInsert.pauseWhileSet();
        }
    } else {
        // adjust pos back to the last document found
        state.pos -= 1;
    }

    return state;
}

void accumulateStats(ECStats& left, const ECStats& right) {
    left.setRead(left.getRead() + right.getRead());
    left.setInserted(left.getInserted() + right.getInserted());
    left.setUpdated(left.getUpdated() + right.getUpdated());
    left.setDeleted(left.getDeleted() + right.getDeleted());
}

void accumulateStats(ECOCStats& left, const ECOCStats& right) {
    left.setRead(left.getRead() + right.getRead());
    left.setDeleted(left.getDeleted() + right.getDeleted());
}

/**
 * Server status section to track an aggregate of global compact statistics.
 */
class FLECompactStatsStatusSection : public ServerStatusSection {
public:
    FLECompactStatsStatusSection() : ServerStatusSection("fle") {
        ECStats zeroStats;
        ECOCStats zeroECOC;

        _stats.setEcc(zeroStats);
        _stats.setEsc(zeroStats);
        _stats.setEcoc(zeroECOC);
    }

    bool includeByDefault() const final {
        return _hasStats;
    }

    BSONObj generateSection(OperationContext* opCtx, const BSONElement& configElement) const final {

        CompactStats temp;
        {
            stdx::lock_guard<Mutex> lock(_mutex);
            temp = _stats;
        }

        BSONObjBuilder builder;
        {
            auto sub = BSONObjBuilder(builder.subobjStart("compactStats"));
            temp.serialize(&sub);
        }

        return builder.obj();
    }

    void updateStats(const CompactStats& stats) {
        stdx::lock_guard<Mutex> lock(_mutex);

        _hasStats = true;
        accumulateStats(_stats.getEsc(), stats.getEsc());
        accumulateStats(_stats.getEcc(), stats.getEcc());
        accumulateStats(_stats.getEcoc(), stats.getEcoc());
    }

private:
    mutable Mutex _mutex = MONGO_MAKE_LATCH("FLECompactStats::_mutex");
    CompactStats _stats;
    bool _hasStats{false};
};

FLECompactStatsStatusSection _fleCompactStatsStatusSection;

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
    namespaces.eccNss =
        NamespaceString(db, cfg.getEccCollection().value_or_eval([&f]() { return f("cache"_sd); }));
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
stdx::unordered_set<ECOCCompactionDocument> getUniqueCompactionDocuments(
    FLEQueryInterface* queryImpl,
    const CompactStructuredEncryptionData& request,
    const NamespaceString& ecocNss,
    ECOCStats* ecocStats) {

    CompactStatsCounter<ECOCStats> stats(ecocStats);

    // Initialize a set 'C' and for each compaction token, find all entries
    // in ECOC with matching field name. Decrypt entries and add to set 'C'.
    stdx::unordered_set<ECOCCompactionDocument> c;
    auto compactionTokens = CompactionHelpers::parseCompactionTokens(request.getCompactionTokens());

    for (auto& compactionToken : compactionTokens) {
        auto docs = queryImpl->findDocuments(
            ecocNss, BSON(EcocDocument::kFieldNameFieldName << compactionToken.fieldPathName));
        stats.addReads(docs.size());

        for (auto& doc : docs) {
            auto ecocDoc = ECOCCollection::parseAndDecrypt(doc, compactionToken.token);
            c.insert(std::move(ecocDoc));
        }
    }
    return c;
}

void compactOneFieldValuePair(FLEQueryInterface* queryImpl,
                              const ECOCCompactionDocument& ecocDoc,
                              const EncryptedStateCollectionsNamespaces& namespaces,
                              ECStats* escStats,
                              ECStats* eccStats) {
    // PART 1
    // prepare the ESC, and get back the highest counter value before the placeholder
    // document, ipos, and pos
    auto escTagToken = FLETwiceDerivedTokenGenerator::generateESCTwiceDerivedTagToken(ecocDoc.esc);
    auto escValueToken =
        FLETwiceDerivedTokenGenerator::generateESCTwiceDerivedValueToken(ecocDoc.esc);
    auto escState =
        prepareESCForCompaction(queryImpl, namespaces.escNss, escTagToken, escValueToken, escStats);

    // PART 2
    // prepare the ECC, and get back the merged set 'g_prime', whether (g_prime != g),
    // ipos_prime, and pos_prime
    auto eccTagToken = FLETwiceDerivedTokenGenerator::generateECCTwiceDerivedTagToken(ecocDoc.ecc);
    auto eccValueToken =
        FLETwiceDerivedTokenGenerator::generateECCTwiceDerivedValueToken(ecocDoc.ecc);
    auto eccState =
        prepareECCForCompaction(queryImpl, namespaces.eccNss, eccTagToken, eccValueToken, eccStats);

    // PART 3
    // A. compact the ECC
    StmtId stmtId = kUninitializedStmtId;

    if (eccState.count != 0) {
        if (eccState.merged) {
            CompactStatsCounter<ECStats> stats(eccStats);

            // a. for each entry in g_prime at index k, insert
            //  {_id: F(eccTagToken, pos'+ k), value: Enc(eccValueToken, g_prime[k])}
            for (auto k = eccState.g_prime.size(); k > 0; k--) {
                const auto& range = eccState.g_prime[k - 1];
                auto insertReply = uassertStatusOK(queryImpl->insertDocument(
                    namespaces.eccNss,
                    ECCCollection::generateDocument(
                        eccTagToken, eccValueToken, eccState.pos + k, range.start, range.end),
                    &stmtId,
                    true));
                checkWriteErrors(insertReply);
                stats.addInserts(1);
            }

            // b & c. update or insert the ECC null doc
            bool hasNullDoc = (eccState.ipos > 1);
            auto newNullDoc =
                ECCCollection::generateNullDocument(eccTagToken, eccValueToken, eccState.pos - 1);
            upsertNullDocument(queryImpl, hasNullDoc, newNullDoc, namespaces.eccNss, eccStats);

            // d. delete entries between ipos' and pos', inclusive
            for (auto k = eccState.ipos; k <= eccState.pos; k++) {
                deleteECCDocument(queryImpl, namespaces.eccNss, k, eccTagToken, eccStats);
            }
        }
    }

    // B. compact the ESC
    if (escState.count != 0) {
        // Delete ESC entries between ipos and pos, inclusive.
        // The compaction placeholder is at index pos, so it will be deleted as well.
        for (auto k = escState.ipos; k <= escState.pos; k++) {
            deleteESCDocument(queryImpl, namespaces.escNss, k, escTagToken, escStats);
        }

        // update or insert the ESC null doc
        bool hasNullDoc = (escState.ipos > 1);
        auto newNullDoc = ESCCollection::generateNullDocument(
            escTagToken, escValueToken, escState.pos - 1, escState.count);
        upsertNullDocument(queryImpl, hasNullDoc, newNullDoc, namespaces.escNss, escStats);
    }
}

CompactStats processFLECompact(OperationContext* opCtx,
                               const CompactStructuredEncryptionData& request,
                               GetTxnCallback getTxn,
                               const EncryptedStateCollectionsNamespaces& namespaces) {
    auto ecocStats = std::make_shared<ECOCStats>();
    auto escStats = std::make_shared<ECStats>();
    auto eccStats = std::make_shared<ECStats>();
    auto c = std::make_shared<stdx::unordered_set<ECOCCompactionDocument>>();

    if (MONGO_unlikely(fleCompactFailBeforeECOCRead.shouldFail())) {
        uasserted(6599901, "Failed compact due to fleCompactFailBeforeECOCRead fail point");
    }

    // Read the ECOC documents in a transaction
    {
        std::shared_ptr<txn_api::SyncTransactionWithRetries> trun = getTxn(opCtx);

        // The function that handles the transaction may outlive this function so we need to use
        // shared_ptrs
        auto argsBlock = std::make_tuple(request, namespaces);
        auto sharedBlock = std::make_shared<decltype(argsBlock)>(argsBlock);

        auto swResult = trun->runNoThrow(
            opCtx,
            [sharedBlock, c, ecocStats](const txn_api::TransactionClient& txnClient,
                                        ExecutorPtr txnExec) {
                FLEQueryInterfaceImpl queryImpl(txnClient, getGlobalServiceContext());

                auto [request2, namespaces2] = *sharedBlock.get();

                *c = getUniqueCompactionDocuments(
                    &queryImpl, request2, namespaces2.ecocRenameNss, ecocStats.get());

                return SemiFuture<void>::makeReady();
            });

        uassertStatusOK(swResult);
        uassertStatusOK(swResult.getValue().getEffectiveStatus());
    }

    // Each entry in 'C' represents a unique field/value pair. For each field/value pair,
    // compact the ESC & ECC entries for that field/value pair in one transaction.
    for (auto& ecocDoc : *c) {
        // start a new transaction
        std::shared_ptr<txn_api::SyncTransactionWithRetries> trun = getTxn(opCtx);

        // The function that handles the transaction may outlive this function so we need to use
        // shared_ptrs
        auto argsBlock = std::make_tuple(ecocDoc, namespaces);
        auto sharedBlock = std::make_shared<decltype(argsBlock)>(argsBlock);

        auto swResult = trun->runNoThrow(
            opCtx,
            [sharedBlock, escStats, eccStats](const txn_api::TransactionClient& txnClient,
                                              ExecutorPtr txnExec) {
                FLEQueryInterfaceImpl queryImpl(txnClient, getGlobalServiceContext());

                auto [ecocDoc2, namespaces2] = *sharedBlock.get();

                compactOneFieldValuePair(
                    &queryImpl, ecocDoc2, namespaces2, escStats.get(), eccStats.get());

                return SemiFuture<void>::makeReady();
            });

        uassertStatusOK(swResult);
        uassertStatusOK(swResult.getValue().getEffectiveStatus());
    }

    CompactStats stats(*ecocStats, *eccStats, *escStats);
    _fleCompactStatsStatusSection.updateStats(stats);

    return stats;
}

void validateCompactRequest(const CompactStructuredEncryptionData& request, const Collection& edc) {
    uassert(6346807,
            "Target namespace is not an encrypted collection",
            edc.getCollectionOptions().encryptedFieldConfig);

    // Validate the request contains a compaction token for each encrypted field
    const auto& efc = edc.getCollectionOptions().encryptedFieldConfig.value();
    CompactionHelpers::validateCompactionTokens(efc, request.getCompactionTokens());
}

}  // namespace mongo
