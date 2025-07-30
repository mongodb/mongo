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

#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/initializer.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/crypto/encryption_fields_gen.h"
#include "mongo/crypto/fle_crypto.h"
#include "mongo/crypto/fle_field_schema_gen.h"
#include "mongo/db/fle_crud.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_geo_near.h"
#include "mongo/db/pipeline/document_source_graph_lookup.h"
#include "mongo/db/pipeline/document_source_lookup.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/query/fle/query_rewriter.h"
#include "mongo/db/query/fle/server_rewrite_helper.h"
#include "mongo/db/service_context.h"
#include "mongo/db/transaction/transaction_api.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/future.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/namespace_string_util.h"

#include <functional>
#include <list>
#include <memory>
#include <string>
#include <typeindex>
#include <utility>

#include <absl/container/node_hash_map.h>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>


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

void rewriteLookUp(QueryRewriter* rewriter, DocumentSourceLookUp* source) {
    if (!feature_flags::gFeatureFlagLookupEncryptionSchemasFLE.isEnabled()) {
        return;
    }
    // When rewriting a lookup, we're only concerned with rewriting the pipeline of the lookup which
    // may contain encrypted placeholders.
    if (!source->hasPipeline()) {
        return;
    }

    bool hadPipelineRewrites{false};
    auto fromExpCtx = source->getSubpipelineExpCtx();
    tassert(9775505, "Invalid from expression context", fromExpCtx);
    // Create a new pipeline rewriter for the foreign collection namespace's pipeline.
    auto subpipelineRewriter = rewriter->createSubpipelineRewriter(source->getFromNs(), fromExpCtx);
    for (auto&& src : source->getResolvedIntrospectionPipeline().getSources()) {
        if (stageRewriterMap.find(typeid(*src)) != stageRewriterMap.end()) {
            hadPipelineRewrites = true;
            stageRewriterMap[typeid(*src)](&subpipelineRewriter, src.get());
        }
    }

    if (hadPipelineRewrites) {
        source->rebuildResolvedPipeline();
    }
}

REGISTER_DOCUMENT_SOURCE_FLE_REWRITER(DocumentSourceMatch, rewriteMatch);
REGISTER_DOCUMENT_SOURCE_FLE_REWRITER(DocumentSourceGeoNear, rewriteGeoNear);
REGISTER_DOCUMENT_SOURCE_FLE_REWRITER(DocumentSourceGraphLookUp, rewriteGraphLookUp);
REGISTER_DOCUMENT_SOURCE_FLE_REWRITER(DocumentSourceLookUp, rewriteLookUp);


BSONObj rewriteEncryptedFilterV2(FLETagQueryInterface* queryImpl,
                                 const NamespaceString& nssEsc,
                                 boost::intrusive_ptr<ExpressionContext> expCtx,
                                 BSONObj filter,
                                 const std::map<NamespaceString, NamespaceString>& escMap,
                                 EncryptedCollScanModeAllowed mode) {

    if (auto rewritten =
            QueryRewriter(expCtx, queryImpl, nssEsc, escMap, mode).rewriteMatchExpression(filter)) {
        return rewritten.value();
    }

    return filter;
}

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
        FLETagNoTXNQuery queryInterface(opCtx);

        sharedBlock->doRewrite(&queryInterface);
        return;
    }
    // This scoped object will stash transaction resource and restore them on commit. This is
    // necessary because the internal transaction will cause the participant to take ownership of
    // the locker in case of yield and destroy it in case of abort. Destroying the locker invariants
    // if some locks are still held. Note that for transactions (where we hold 2-phase locks)
    // stashing locks doesn't directly release them but signals the locker it can destroy them if
    // necessary.
    auto stashHandle = StashTransactionResourcesForMultiDocumentTransaction(opCtx);
    auto txn = getTxn(opCtx);
    auto service = opCtx->getService();
    auto swCommitResult = txn->runNoThrow(
        opCtx, [service, sharedBlock](const txn_api::TransactionClient& txnClient, auto txnExec) {
            // Construct FLE rewriter from the transaction client and encryptionInformation.
            auto queryInterface = FLEQueryInterfaceImpl(txnClient, service);

            // Rewrite the MatchExpression.
            sharedBlock->doRewrite(&queryInterface);

            return SemiFuture<void>::makeReady();
        });

    uassertStatusOK(swCommitResult);
    uassertStatusOK(swCommitResult.getValue().cmdStatus);
    uassertStatusOK(swCommitResult.getValue().getEffectiveStatus());
    stashHandle.restoreOnCommit();
}

NamespaceString getAndValidateEscNsFromSchema(const EncryptionInformation& encryptInfo,
                                              const NamespaceString& nss,
                                              bool allowEmptySchema) {
    // In the case of PipelineRewrite, we must allow for unencrypted schemas alongside QE schemas,
    // which manifest as collections without schemas in the provided encryptionInformation.
    if (allowEmptySchema &&
        !encryptInfo.getSchema().hasField(nss.serializeWithoutTenantPrefix_UNSAFE())) {
        return NamespaceString();
    }
    auto efc = EncryptionInformationHelpers::getAndValidateSchema(nss, encryptInfo);
    return NamespaceStringUtil::deserialize(nss.dbName(), std::string{*efc.getEscCollection()});
}

std::map<NamespaceString, NamespaceString> generateEncryptInfoEscMap(
    const DatabaseName& dbName, const EncryptionInformation& encryptInfo) {
    std::map<NamespaceString, NamespaceString> escMap;
    if (feature_flags::gFeatureFlagLookupEncryptionSchemasFLE.isEnabled()) {
        // Get the Esc collection namespace for every namespace in our encryption schema.
        for (const auto& elem : encryptInfo.getSchema()) {
            uassert(9775500,
                    "Each namespace schema "
                    "must be an object",
                    elem.type() == BSONType::object);
            auto schemaNs = NamespaceStringUtil::deserialize(
                boost::none, elem.fieldNameStringData(), SerializationContext::stateDefault());
            auto efc = EncryptionInformationHelpers::getAndValidateSchema(schemaNs, encryptInfo);

            escMap.emplace(std::piecewise_construct,
                           std::forward_as_tuple(std::move(schemaNs)),
                           std::forward_as_tuple(NamespaceStringUtil::deserialize(
                               dbName, std::string{*efc.getEscCollection()})));
        }
    }
    return escMap;
}
}  // namespace

RewriteBase::RewriteBase(boost::intrusive_ptr<ExpressionContext> expCtx,
                         const NamespaceString& nss,
                         const EncryptionInformation& encryptInfo,
                         bool allowEmptySchema)
    : expCtx(expCtx),
      nssEsc(getAndValidateEscNsFromSchema(encryptInfo, nss, allowEmptySchema)),
      _escMap(generateEncryptInfoEscMap(nss.dbName(), encryptInfo)) {}

FilterRewrite::FilterRewrite(boost::intrusive_ptr<ExpressionContext> expCtx,
                             const NamespaceString& nss,
                             const EncryptionInformation& encryptInfo,
                             BSONObj toRewrite,
                             EncryptedCollScanModeAllowed mode)
    : RewriteBase(expCtx, nss, encryptInfo, false), userFilter(toRewrite), _mode(mode) {}

void FilterRewrite::doRewrite(FLETagQueryInterface* queryImpl) {
    rewrittenFilter =
        rewriteEncryptedFilterV2(queryImpl, nssEsc, expCtx, userFilter, _escMap, _mode);
}

PipelineRewrite::PipelineRewrite(const NamespaceString& nss,
                                 const EncryptionInformation& encryptInfo,
                                 std::unique_ptr<Pipeline> toRewrite)
    : RewriteBase(toRewrite->getContext(), nss, encryptInfo, true),
      _pipeline(std::move(toRewrite)) {}

void PipelineRewrite::doRewrite(FLETagQueryInterface* queryImpl) {
    auto rewriter = getQueryRewriterForEsc(queryImpl);
    for (auto&& source : _pipeline->getSources()) {
        if (stageRewriterMap.find(typeid(*source)) != stageRewriterMap.end()) {
            stageRewriterMap[typeid(*source)](&rewriter, source.get());
        }
    }
}

std::unique_ptr<Pipeline> PipelineRewrite::getPipeline() {
    return std::move(_pipeline);
}

QueryRewriter PipelineRewrite::getQueryRewriterForEsc(FLETagQueryInterface* queryImpl) {
    return QueryRewriter(expCtx, queryImpl, nssEsc, _escMap);
}

BSONObj rewriteEncryptedFilterInsideTxn(FLETagQueryInterface* queryImpl,
                                        const NamespaceString& nss,
                                        const EncryptedFieldConfig& efc,
                                        boost::intrusive_ptr<ExpressionContext> expCtx,
                                        BSONObj filter,
                                        EncryptedCollScanModeAllowed mode) {
    NamespaceString nssEsc(
        NamespaceStringUtil::deserialize(nss.dbName(), efc.getEscCollection().value()));

    return rewriteEncryptedFilterV2(queryImpl, nssEsc, expCtx, filter, {{nss, nssEsc}}, mode);
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
    auto expCtx = ExpressionContextBuilder{}
                      .fromRequest(opCtx, *findCommand)
                      .collator(collatorFromBSON(opCtx, findCommand->getCollation()))
                      .ns(nss)
                      .build();
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
    auto expCtx =
        ExpressionContextBuilder{}
            .opCtx(opCtx)
            .collator(collatorFromBSON(opCtx, countCommand->getCollation().value_or(BSONObj())))
            .ns(nss)
            .build();

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

std::unique_ptr<Pipeline> processPipeline(OperationContext* opCtx,
                                          NamespaceString nss,
                                          const EncryptionInformation& encryptInfo,
                                          std::unique_ptr<Pipeline> toRewrite,
                                          GetTxnCallback txn) {
    auto sharedBlock = std::make_shared<PipelineRewrite>(nss, encryptInfo, std::move(toRewrite));
    doFLERewriteInTxn(opCtx, sharedBlock, txn);

    return sharedBlock->getPipeline();
}
}  // namespace mongo::fle
