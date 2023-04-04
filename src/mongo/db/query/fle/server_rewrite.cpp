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


#include "mongo/db/query/fle/server_rewrite.h"

#include <memory>

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/crypto/encryption_fields_gen.h"
#include "mongo/crypto/fle_crypto.h"
#include "mongo/crypto/fle_field_schema_gen.h"
#include "mongo/crypto/fle_tags.h"
#include "mongo/db/fle_crud.h"
#include "mongo/db/matcher/expression_expr.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/document_source_geo_near.h"
#include "mongo/db/pipeline/document_source_graph_lookup.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/query/fle/encrypted_predicate.h"
#include "mongo/db/query/fle/query_rewriter.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"
#include "mongo/s/grid.h"
#include "mongo/s/transaction_router_resource_yielder.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"


#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo::fle {

// TODO: This is a generally useful helper function that should probably go in some other namespace.
std::unique_ptr<CollatorInterface> collatorFromBSON(OperationContext* opCtx,
                                                    const BSONObj& collation) {
    std::unique_ptr<CollatorInterface> collator;
    if (!collation.isEmpty()) {
        auto statusWithCollator =
            CollatorFactoryInterface::get(opCtx->getServiceContext())->makeFromBSON(collation);
        uassertStatusOK(statusWithCollator.getStatus());
        collator = std::move(statusWithCollator.getValue());
    }
    return collator;
}
namespace {
/**
 * This section defines a mapping from DocumentSources to the dispatch function to appropriately
 * handle FLE rewriting for that stage. This should be kept in line with code on the client-side
 * that marks constants for encryption: we should handle all places where an implicitly-encrypted
 * value may be for each stage, otherwise we may return non-sensical results.
 */
static stdx::unordered_map<std::type_index, std::function<void(QueryRewriter*, DocumentSource*)>>
    stageRewriterMap;

#define REGISTER_DOCUMENT_SOURCE_FLE_REWRITER(className, rewriterFunc)                 \
    MONGO_INITIALIZER(encryptedAnalyzerFor_##className)(InitializerContext*) {         \
                                                                                       \
        invariant(stageRewriterMap.find(typeid(className)) == stageRewriterMap.end()); \
        stageRewriterMap[typeid(className)] = [&](auto* rewriter, auto* source) {      \
            rewriterFunc(rewriter, static_cast<className*>(source));                   \
        };                                                                             \
    }

void rewriteMatch(QueryRewriter* rewriter, DocumentSourceMatch* source) {
    if (auto rewritten = rewriter->rewriteMatchExpression(source->getQuery())) {
        source->rebuild(rewritten.value());
    }
}

void rewriteGeoNear(QueryRewriter* rewriter, DocumentSourceGeoNear* source) {
    if (auto rewritten = rewriter->rewriteMatchExpression(source->getQuery())) {
        source->setQuery(rewritten.value());
    }
}

void rewriteGraphLookUp(QueryRewriter* rewriter, DocumentSourceGraphLookUp* source) {
    if (auto filter = source->getAdditionalFilter()) {
        if (auto rewritten = rewriter->rewriteMatchExpression(filter.value())) {
            source->setAdditionalFilter(rewritten.value());
        }
    }

    if (auto newExpr = rewriter->rewriteExpression(source->getStartWithField())) {
        source->setStartWithField(newExpr.release());
    }
}

REGISTER_DOCUMENT_SOURCE_FLE_REWRITER(DocumentSourceMatch, rewriteMatch);
REGISTER_DOCUMENT_SOURCE_FLE_REWRITER(DocumentSourceGeoNear, rewriteGeoNear);
REGISTER_DOCUMENT_SOURCE_FLE_REWRITER(DocumentSourceGraphLookUp, rewriteGraphLookUp);


BSONObj rewriteEncryptedFilterV2(FLETagQueryInterface* queryImpl,
                                 const NamespaceString& nssEsc,
                                 boost::intrusive_ptr<ExpressionContext> expCtx,
                                 BSONObj filter,
                                 EncryptedCollScanModeAllowed mode) {

    if (auto rewritten =
            QueryRewriter(expCtx, queryImpl, nssEsc, mode).rewriteMatchExpression(filter)) {
        return rewritten.value();
    }

    return filter;
}


class RewriteBase {
public:
    RewriteBase(boost::intrusive_ptr<ExpressionContext> expCtx,
                const NamespaceString& nss,
                const EncryptionInformation& encryptInfo)
        : expCtx(expCtx), dbName(nss.dbName()) {
        auto efc = EncryptionInformationHelpers::getAndValidateSchema(nss, encryptInfo);
        esc = efc.getEscCollection()->toString();
    }
    virtual ~RewriteBase(){};

    virtual void doRewrite(FLETagQueryInterface* queryImpl, const NamespaceString& nssEsc){};


    boost::intrusive_ptr<ExpressionContext> expCtx;
    std::string esc;
    DatabaseName dbName;
};

// This class handles rewriting of an entire pipeline.
class PipelineRewrite : public RewriteBase {
public:
    PipelineRewrite(const NamespaceString& nss,
                    const EncryptionInformation& encryptInfo,
                    std::unique_ptr<Pipeline, PipelineDeleter> toRewrite)
        : RewriteBase(toRewrite->getContext(), nss, encryptInfo), pipeline(std::move(toRewrite)) {}

    ~PipelineRewrite(){};

    void doRewrite(FLETagQueryInterface* queryImpl, const NamespaceString& nssEsc) final {
        auto rewriter = QueryRewriter(expCtx, queryImpl, nssEsc);
        for (auto&& source : pipeline->getSources()) {
            if (stageRewriterMap.find(typeid(*source)) != stageRewriterMap.end()) {
                stageRewriterMap[typeid(*source)](&rewriter, source.get());
            }
        }
    }

    std::unique_ptr<Pipeline, PipelineDeleter> getPipeline() {
        return std::move(pipeline);
    }

private:
    std::unique_ptr<Pipeline, PipelineDeleter> pipeline;
};

// This class handles rewriting of a single match expression, represented as a BSONObj.
class FilterRewrite : public RewriteBase {
public:
    FilterRewrite(boost::intrusive_ptr<ExpressionContext> expCtx,
                  const NamespaceString& nss,
                  const EncryptionInformation& encryptInfo,
                  const BSONObj toRewrite,
                  EncryptedCollScanModeAllowed mode)
        : RewriteBase(expCtx, nss, encryptInfo), userFilter(toRewrite), _mode(mode) {}

    ~FilterRewrite(){};

    void doRewrite(FLETagQueryInterface* queryImpl, const NamespaceString& nssEsc) final {
        rewrittenFilter = rewriteEncryptedFilterV2(queryImpl, nssEsc, expCtx, userFilter, _mode);
    }

    const BSONObj userFilter;
    BSONObj rewrittenFilter;
    EncryptedCollScanModeAllowed _mode;
};

// This helper executes the rewrite(s) inside a transaction. The transaction runs in a separate
// executor, and so we can't pass data by reference into the lambda. The provided rewriter should
// hold all the data we need to do the rewriting inside the lambda, and is passed in a more
// threadsafe shared_ptr. The result of applying the rewrites can be accessed in the RewriteBase.
void doFLERewriteInTxn(OperationContext* opCtx,
                       std::shared_ptr<RewriteBase> sharedBlock,
                       GetTxnCallback getTxn) {

    // This code path only works if we are NOT running in a a transaction.
    // if breaks us off of the current optctx readconcern and other settings
    //
    if (!opCtx->inMultiDocumentTransaction()) {
        NamespaceString nssEsc(
            NamespaceStringUtil::parseNamespaceFromRequest(sharedBlock->dbName, sharedBlock->esc));
        FLETagNoTXNQuery queryInterface(opCtx);

        sharedBlock->doRewrite(&queryInterface, nssEsc);
        return;
    }

    auto txn = getTxn(opCtx);
    auto swCommitResult = txn->runNoThrow(
        opCtx, [sharedBlock](const txn_api::TransactionClient& txnClient, auto txnExec) {
            NamespaceString nssEsc(NamespaceStringUtil::parseNamespaceFromRequest(
                sharedBlock->dbName, sharedBlock->esc));

            // Construct FLE rewriter from the transaction client and encryptionInformation.
            auto queryInterface = FLEQueryInterfaceImpl(txnClient, getGlobalServiceContext());

            // Rewrite the MatchExpression.
            sharedBlock->doRewrite(&queryInterface, nssEsc);

            return SemiFuture<void>::makeReady();
        });

    uassertStatusOK(swCommitResult);
    uassertStatusOK(swCommitResult.getValue().cmdStatus);
    uassertStatusOK(swCommitResult.getValue().getEffectiveStatus());
}
}  // namespace

BSONObj rewriteEncryptedFilterInsideTxn(FLETagQueryInterface* queryImpl,
                                        const DatabaseName& dbName,
                                        const EncryptedFieldConfig& efc,
                                        boost::intrusive_ptr<ExpressionContext> expCtx,
                                        BSONObj filter,
                                        EncryptedCollScanModeAllowed mode) {
    NamespaceString nssEsc(
        NamespaceStringUtil::parseNamespaceFromRequest(dbName, efc.getEscCollection().value()));

    return rewriteEncryptedFilterV2(queryImpl, nssEsc, expCtx, filter, mode);
}

BSONObj rewriteQuery(OperationContext* opCtx,
                     boost::intrusive_ptr<ExpressionContext> expCtx,
                     const NamespaceString& nss,
                     const EncryptionInformation& info,
                     BSONObj filter,
                     GetTxnCallback getTransaction,
                     EncryptedCollScanModeAllowed mode) {
    auto sharedBlock = std::make_shared<FilterRewrite>(expCtx, nss, info, filter, mode);
    doFLERewriteInTxn(opCtx, sharedBlock, getTransaction);
    return sharedBlock->rewrittenFilter.getOwned();
}


void processFindCommand(OperationContext* opCtx,
                        const NamespaceString& nss,
                        FindCommandRequest* findCommand,
                        GetTxnCallback getTransaction) {
    invariant(findCommand->getEncryptionInformation());

    auto expCtx =
        make_intrusive<ExpressionContext>(opCtx,
                                          collatorFromBSON(opCtx, findCommand->getCollation()),
                                          nss,
                                          findCommand->getLegacyRuntimeConstants(),
                                          findCommand->getLet());
    expCtx->stopExpressionCounters();
    findCommand->setFilter(rewriteQuery(opCtx,
                                        expCtx,
                                        nss,
                                        findCommand->getEncryptionInformation().value(),
                                        findCommand->getFilter().getOwned(),
                                        getTransaction,
                                        EncryptedCollScanModeAllowed::kAllow));

    findCommand->getEncryptionInformation()->setCrudProcessed(true);
}

void processCountCommand(OperationContext* opCtx,
                         const NamespaceString& nss,
                         CountCommandRequest* countCommand,
                         GetTxnCallback getTxn) {
    invariant(countCommand->getEncryptionInformation());
    // Count command does not have legacy runtime constants, and does not support user variables
    // defined in a let expression.
    auto expCtx = make_intrusive<ExpressionContext>(
        opCtx, collatorFromBSON(opCtx, countCommand->getCollation().value_or(BSONObj())), nss);
    expCtx->stopExpressionCounters();

    countCommand->setQuery(rewriteQuery(opCtx,
                                        expCtx,
                                        nss,
                                        countCommand->getEncryptionInformation().value(),
                                        countCommand->getQuery().getOwned(),
                                        getTxn,
                                        EncryptedCollScanModeAllowed::kAllow));

    countCommand->getEncryptionInformation()->setCrudProcessed(true);
}

std::unique_ptr<Pipeline, PipelineDeleter> processPipeline(
    OperationContext* opCtx,
    NamespaceString nss,
    const EncryptionInformation& encryptInfo,
    std::unique_ptr<Pipeline, PipelineDeleter> toRewrite,
    GetTxnCallback txn) {

    auto sharedBlock = std::make_shared<PipelineRewrite>(nss, encryptInfo, std::move(toRewrite));
    doFLERewriteInTxn(opCtx, sharedBlock, txn);

    return sharedBlock->getPipeline();
}
}  // namespace mongo::fle
