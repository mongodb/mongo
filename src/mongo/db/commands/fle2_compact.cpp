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

// TODO: SERVER-73303 delete the below failpoints when v2 is enabled by default
MONGO_FAIL_POINT_DEFINE(fleCompactHangBeforeESCPlaceholderInsert);
MONGO_FAIL_POINT_DEFINE(fleCompactHangAfterESCPlaceholderInsert);
MONGO_FAIL_POINT_DEFINE(fleCompactHangBeforeECCPlaceholderInsert);
MONGO_FAIL_POINT_DEFINE(fleCompactHangAfterECCPlaceholderInsert);

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

// TODO: SERVER-73303 delete when v2 is enabled by default
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

// TODO: SERVER-73303 delete when v2 is enabled by default
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
        auto reply = uassertStatusOK(queryImpl->insertDocuments(nss, {newNullDoc}, &stmtId, true));
        checkWriteErrors(reply);
        statsCtr.addInserts(1);
    }
}

// TODO: SERVER-73303 delete when v2 is enabled by default
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

// TODO: SERVER-73303 delete when v2 is enabled by default
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

// TODO: SERVER-73303 delete when v2 is enabled by default
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

// TODO: SERVER-73303 delete when v2 is enabled by default
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

    TxnCollectionReaderForCompact reader(queryImpl, nssEsc, escStats);

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
        uassertStatusOK(queryImpl->insertDocuments(nssEsc, {placeholder}, &stmtId, true));
    checkWriteErrors(insertReply);
    stats.addInserts(1);

    if (MONGO_unlikely(fleCompactHangAfterESCPlaceholderInsert.shouldFail())) {
        LOGV2(6548302, "Hanging due to fleCompactHangAfterESCPlaceholderInsert fail point");
        fleCompactHangAfterESCPlaceholderInsert.pauseWhileSet();
    }
    return state;
}

// TODO: SERVER-73303 delete when v2 is enabled by default
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

// TODO: SERVER-73303 delete when v2 is enabled by default
ECCPreCompactState prepareECCForCompaction(FLEQueryInterface* queryImpl,
                                           const NamespaceString& nssEcc,
                                           const ECCTwiceDerivedTagToken& tagToken,
                                           const ECCTwiceDerivedValueToken& valueToken,
                                           ECStats* eccStats) {
    CompactStatsCounter<ECStats> stats(eccStats);

    TxnCollectionReaderForCompact reader(queryImpl, nssEcc, eccStats);

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
            uassertStatusOK(queryImpl->insertDocuments(nssEcc, {placeholder}, &stmtId, true));
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

    // TODO SERVER-73303 remove when feature flag is enabled.
    if (!gFeatureFlagFLE2ProtocolVersion2.isEnabled(serverGlobalParams.featureCompatibility)) {
        namespaces.eccNss = NamespaceString(
            db, cfg.getEccCollection().value_or_eval([&f]() { return f("cache"_sd); }));
    }

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
                auto insertReply = uassertStatusOK(queryImpl->insertDocuments(
                    namespaces.eccNss,
                    {ECCCollection::generateDocument(
                        eccTagToken, eccValueToken, eccState.pos + k, range.start, range.end)},
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

    CompactStats stats(*ecocStats, *escStats);
    stats.setEcc(*eccStats);
    FLEStatusSection::get().updateCompactionStats(stats);

    return stats;
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
