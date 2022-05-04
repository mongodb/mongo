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
#include "mongo/db/service_context.h"
#include "mongo/s/grid.h"
#include "mongo/s/transaction_router_resource_yielder.h"
#include "mongo/util/assert_util.h"

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
static stdx::unordered_map<std::type_index, std::function<void(FLEQueryRewriter*, DocumentSource*)>>
    stageRewriterMap;

#define REGISTER_DOCUMENT_SOURCE_FLE_REWRITER(className, rewriterFunc)                 \
    MONGO_INITIALIZER(encryptedAnalyzerFor_##className)(InitializerContext*) {         \
                                                                                       \
        invariant(stageRewriterMap.find(typeid(className)) == stageRewriterMap.end()); \
        stageRewriterMap[typeid(className)] = [&](auto* rewriter, auto* source) {      \
            rewriterFunc(rewriter, static_cast<className*>(source));                   \
        };                                                                             \
    }

void rewriteMatch(FLEQueryRewriter* rewriter, DocumentSourceMatch* source) {
    if (auto rewritten = rewriter->rewriteMatchExpression(source->getQuery())) {
        source->rebuild(rewritten.get());
    }
}

void rewriteGeoNear(FLEQueryRewriter* rewriter, DocumentSourceGeoNear* source) {
    if (auto rewritten = rewriter->rewriteMatchExpression(source->getQuery())) {
        source->setQuery(rewritten.get());
    }
}

void rewriteGraphLookUp(FLEQueryRewriter* rewriter, DocumentSourceGraphLookUp* source) {
    if (auto filter = source->getAdditionalFilter()) {
        if (auto rewritten = rewriter->rewriteMatchExpression(filter.get())) {
            source->setAdditionalFilter(rewritten.get());
        }
    }

    if (auto newExpr = rewriter->rewriteExpression(source->getStartWithField())) {
        source->setStartWithField(newExpr.release());
    }
}

REGISTER_DOCUMENT_SOURCE_FLE_REWRITER(DocumentSourceMatch, rewriteMatch);
REGISTER_DOCUMENT_SOURCE_FLE_REWRITER(DocumentSourceGeoNear, rewriteGeoNear);
REGISTER_DOCUMENT_SOURCE_FLE_REWRITER(DocumentSourceGraphLookUp, rewriteGraphLookUp);

class FLEExpressionRewriter {
public:
    FLEExpressionRewriter(FLEQueryRewriter* queryRewriter) : queryRewriter(queryRewriter){};

    /**
     * Accepts a vector of expressions to be compared for equality to an encrypted field. For any
     * expression representing a constant encrypted value, computes the tags for the expression and
     * rewrites the comparison to a disjunction over __safeContent__. Returns an OR expression of
     * these disjunctions. If no rewrites were done, returns nullptr. Either all of the expressions
     * be constant FFPs or none of them should be.
     *
     * The final output will look like
     * {$or: [{$in: [tag0, "$__safeContent__"]}, {$in: [tag1, "$__safeContent__"]}, ...]}.
     */
    std::unique_ptr<Expression> rewriteComparisonsToEncryptedField(
        const std::vector<boost::intrusive_ptr<Expression>>& equalitiesList) {
        size_t numFFPs = 0;
        std::vector<boost::intrusive_ptr<Expression>> orListElems;

        for (auto& equality : equalitiesList) {
            // For each expression representing a FleFindPayload...
            if (auto constChild = dynamic_cast<ExpressionConstant*>(equality.get())) {
                if (!queryRewriter->isFleFindPayload(constChild->getValue())) {
                    continue;
                }

                // ... rewrite the payload to a list of tags...
                numFFPs++;
                auto tags = queryRewriter->rewritePayloadAsTags(constChild->getValue());
                for (auto&& tagElt : tags) {
                    // ... and for each tag, construct expression {$in: [tag, "$__safeContent__"]}.
                    std::vector<boost::intrusive_ptr<Expression>> inVec{
                        ExpressionConstant::create(queryRewriter->expCtx(), tagElt),
                        ExpressionFieldPath::createPathFromString(
                            queryRewriter->expCtx(),
                            kSafeContent,
                            queryRewriter->expCtx()->variablesParseState)};
                    orListElems.push_back(
                        make_intrusive<ExpressionIn>(queryRewriter->expCtx(), std::move(inVec)));
                }
            }
        }

        // Finally, construct an $or of all of the $ins.
        if (numFFPs == 0) {
            return nullptr;
        }
        uassert(
            6334102,
            "If any elements in an comparison expression are encrypted, then all elements should "
            "be encrypted.",
            numFFPs == equalitiesList.size());

        didRewrite = true;
        return std::make_unique<ExpressionOr>(queryRewriter->expCtx(), std::move(orListElems));
    }

    std::unique_ptr<Expression> postVisit(Expression* exp) {
        if (auto inExpr = dynamic_cast<ExpressionIn*>(exp)) {
            // Rewrite an $in over an encrypted field to an $or. The first child of the $in can be
            // ignored when rewrites are done; there is no extra information in that child that
            // doesn't exist in the FFPs in the $in list.
            if (auto inList = dynamic_cast<ExpressionArray*>(inExpr->getOperandList()[1].get())) {
                return rewriteComparisonsToEncryptedField(inList->getChildren());
            }
        } else if (auto eqExpr = dynamic_cast<ExpressionCompare*>(exp); eqExpr &&
                   (eqExpr->getOp() == ExpressionCompare::EQ ||
                    eqExpr->getOp() == ExpressionCompare::NE)) {
            // Rewrite an $eq comparing an encrypted field and an encrypted constant to an $or.
            // Either child may be the constant, so try rewriting both.
            auto or0 = rewriteComparisonsToEncryptedField({eqExpr->getChildren()[0]});
            auto or1 = rewriteComparisonsToEncryptedField({eqExpr->getChildren()[1]});
            uassert(6334100, "Cannot compare two encrypted constants to each other", !or0 || !or1);

            // Neither child is an encrypted constant, and no rewriting needs to be done.
            if (!or0 && !or1) {
                return nullptr;
            }

            // Exactly one child was an encrypted constant. The other child can be ignored; there is
            // no extra information in that child that doesn't exist in the FFP.
            if (eqExpr->getOp() == ExpressionCompare::NE) {
                std::vector<boost::intrusive_ptr<Expression>> notChild{(or0 ? or0 : or1).release()};
                return std::make_unique<ExpressionNot>(queryRewriter->expCtx(),
                                                       std::move(notChild));
            }
            return std::move(or0 ? or0 : or1);
        }

        return nullptr;
    }

    FLEQueryRewriter* queryRewriter;
    bool didRewrite = false;
};

BSONObj rewriteEncryptedFilter(const FLEStateCollectionReader& escReader,
                               const FLEStateCollectionReader& eccReader,
                               boost::intrusive_ptr<ExpressionContext> expCtx,
                               BSONObj filter) {
    if (auto rewritten =
            FLEQueryRewriter(expCtx, escReader, eccReader).rewriteMatchExpression(filter)) {
        return rewritten.get();
    }
    return filter;
}

class RewriteBase {
public:
    RewriteBase(boost::intrusive_ptr<ExpressionContext> expCtx,
                const NamespaceString& nss,
                const EncryptionInformation& encryptInfo)
        : expCtx(expCtx), db(nss.db()) {
        auto efc = EncryptionInformationHelpers::getAndValidateSchema(nss, encryptInfo);
        esc = efc.getEscCollection()->toString();
        ecc = efc.getEccCollection()->toString();
    }
    virtual ~RewriteBase(){};
    virtual void doRewrite(FLEStateCollectionReader& escReader,
                           FLEStateCollectionReader& eccReader){};

    boost::intrusive_ptr<ExpressionContext> expCtx;
    std::string esc;
    std::string ecc;
    std::string db;
};

// This class handles rewriting of an entire pipeline.
class PipelineRewrite : public RewriteBase {
public:
    PipelineRewrite(const NamespaceString& nss,
                    const EncryptionInformation& encryptInfo,
                    std::unique_ptr<Pipeline, PipelineDeleter> toRewrite)
        : RewriteBase(toRewrite->getContext(), nss, encryptInfo), pipeline(std::move(toRewrite)) {}

    ~PipelineRewrite(){};
    void doRewrite(FLEStateCollectionReader& escReader, FLEStateCollectionReader& eccReader) final {
        auto rewriter = FLEQueryRewriter(expCtx, escReader, eccReader);
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
                  const BSONObj toRewrite)
        : RewriteBase(expCtx, nss, encryptInfo), userFilter(toRewrite) {}

    ~FilterRewrite(){};
    void doRewrite(FLEStateCollectionReader& escReader, FLEStateCollectionReader& eccReader) final {
        rewrittenFilter = rewriteEncryptedFilter(escReader, eccReader, expCtx, userFilter);
    }

    const BSONObj userFilter;
    BSONObj rewrittenFilter;
};

// This helper executes the rewrite(s) inside a transaction. The transaction runs in a separate
// executor, and so we can't pass data by reference into the lambda. The provided rewriter should
// hold all the data we need to do the rewriting inside the lambda, and is passed in a more
// threadsafe shared_ptr. The result of applying the rewrites can be accessed in the RewriteBase.
void doFLERewriteInTxn(OperationContext* opCtx,
                       std::shared_ptr<RewriteBase> sharedBlock,
                       GetTxnCallback getTxn) {
    auto txn = getTxn(opCtx);

    auto swCommitResult = txn->runNoThrow(
        opCtx, [sharedBlock](const txn_api::TransactionClient& txnClient, auto txnExec) {
            auto makeCollectionReader = [sharedBlock](FLEQueryInterface* queryImpl,
                                                      const StringData& coll) {
                NamespaceString nss(sharedBlock->db, coll);
                auto docCount = queryImpl->countDocuments(nss);
                return TxnCollectionReader(docCount, queryImpl, nss);
            };

            // Construct FLE rewriter from the transaction client and encryptionInformation.
            auto queryInterface = FLEQueryInterfaceImpl(txnClient, getGlobalServiceContext());
            auto escReader = makeCollectionReader(&queryInterface, sharedBlock->esc);
            auto eccReader = makeCollectionReader(&queryInterface, sharedBlock->ecc);

            // Rewrite the MatchExpression.
            sharedBlock->doRewrite(escReader, eccReader);

            return SemiFuture<void>::makeReady();
        });

    uassertStatusOK(swCommitResult);
    uassertStatusOK(swCommitResult.getValue().cmdStatus);
    uassertStatusOK(swCommitResult.getValue().getEffectiveStatus());
}
}  // namespace

BSONObj rewriteEncryptedFilterInsideTxn(FLEQueryInterface* queryImpl,
                                        StringData db,
                                        const EncryptedFieldConfig& efc,
                                        boost::intrusive_ptr<ExpressionContext> expCtx,
                                        BSONObj filter) {
    auto makeCollectionReader = [&](FLEQueryInterface* queryImpl, const StringData& coll) {
        NamespaceString nss(db, coll);
        auto docCount = queryImpl->countDocuments(nss);
        return TxnCollectionReader(docCount, queryImpl, nss);
    };
    auto escReader = makeCollectionReader(queryImpl, efc.getEscCollection().get());
    auto eccReader = makeCollectionReader(queryImpl, efc.getEccCollection().get());
    return rewriteEncryptedFilter(escReader, eccReader, expCtx, filter);
}

BSONObj rewriteQuery(OperationContext* opCtx,
                     boost::intrusive_ptr<ExpressionContext> expCtx,
                     const NamespaceString& nss,
                     const EncryptionInformation& info,
                     BSONObj filter,
                     GetTxnCallback getTransaction) {
    auto sharedBlock = std::make_shared<FilterRewrite>(expCtx, nss, info, filter);
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
                                        findCommand->getEncryptionInformation().get(),
                                        findCommand->getFilter().getOwned(),
                                        getTransaction));
    // The presence of encryptionInformation is a signal that this is a FLE request that requires
    // special processing. Once we've rewritten the query, it's no longer a "special" FLE query, but
    // a normal query that can be executed by the query system like any other, so remove
    // encryptionInformation.
    findCommand->setEncryptionInformation(boost::none);
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
                                        countCommand->getEncryptionInformation().get(),
                                        countCommand->getQuery().getOwned(),
                                        getTxn));
    // The presence of encryptionInformation is a signal that this is a FLE request that requires
    // special processing. Once we've rewritten the query, it's no longer a "special" FLE query, but
    // a normal query that can be executed by the query system like any other, so remove
    // encryptionInformation.
    countCommand->setEncryptionInformation(boost::none);
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

std::unique_ptr<Expression> FLEQueryRewriter::rewriteExpression(Expression* expression) {
    tassert(6334104, "Expected an expression to rewrite but found none", expression);

    FLEExpressionRewriter expressionRewriter{this};
    auto res = expression_walker::walk<Expression>(expression, &expressionRewriter);
    _rewroteLastExpression = expressionRewriter.didRewrite;
    return res;
}

boost::optional<BSONObj> FLEQueryRewriter::rewriteMatchExpression(const BSONObj& filter) {
    auto expr = uassertStatusOK(MatchExpressionParser::parse(filter, _expCtx));

    _rewroteLastExpression = false;
    if (auto res = _rewrite(expr.get())) {
        // The rewrite resulted in top-level changes. Serialize the new expression.
        return res->serialize().getOwned();
    } else if (_rewroteLastExpression) {
        // The rewrite had no top-level changes, but nested expressions were rewritten. Serialize
        // the parsed expression, which has in-place changes.
        return expr->serialize().getOwned();
    }

    // No rewrites were done.
    return boost::none;
}

std::unique_ptr<MatchExpression> FLEQueryRewriter::_rewrite(MatchExpression* expr) {
    switch (expr->matchType()) {
        case MatchExpression::EQ:
            return rewriteEq(std::move(static_cast<const EqualityMatchExpression*>(expr)));
        case MatchExpression::MATCH_IN:
            return rewriteIn(std::move(static_cast<const InMatchExpression*>(expr)));
        case MatchExpression::AND:
        case MatchExpression::OR:
        case MatchExpression::NOT:
        case MatchExpression::NOR: {
            for (size_t i = 0; i < expr->numChildren(); i++) {
                auto child = expr->getChild(i);
                if (auto newChild = _rewrite(child)) {
                    expr->resetChild(i, newChild.release());
                }
            }
            return nullptr;
        }
        case MatchExpression::EXPRESSION: {
            // Save the current value of _rewroteLastExpression, since rewriteExpression() may
            // reset it to false and we may have already done a match expression rewrite.
            auto didRewrite = _rewroteLastExpression;
            auto rewritten =
                rewriteExpression(static_cast<ExprMatchExpression*>(expr)->getExpression().get());
            _rewroteLastExpression |= didRewrite;
            if (rewritten) {
                return std::make_unique<ExprMatchExpression>(rewritten.release(), expCtx());
            }
            [[fallthrough]];
        }
        default:
            return nullptr;
    }
}

BSONObj FLEQueryRewriter::rewritePayloadAsTags(BSONElement fleFindPayload) const {
    auto tokens = ParsedFindPayload(fleFindPayload);
    auto tags = readTags(*_escReader,
                         *_eccReader,
                         tokens.escToken,
                         tokens.eccToken,
                         tokens.edcToken,
                         tokens.maxCounter);

    auto bab = BSONArrayBuilder();
    for (auto tag : tags) {
        bab.appendBinData(tag.size(), BinDataType::BinDataGeneral, tag.data());
    }

    return bab.obj().getOwned();
}

std::vector<Value> FLEQueryRewriter::rewritePayloadAsTags(Value fleFindPayload) const {
    auto tokens = ParsedFindPayload(fleFindPayload);
    auto tags = readTags(*_escReader,
                         *_eccReader,
                         tokens.escToken,
                         tokens.eccToken,
                         tokens.edcToken,
                         tokens.maxCounter);

    std::vector<Value> tagVec;
    for (auto tag : tags) {
        tagVec.push_back(Value(BSONBinData(tag.data(), tag.size(), BinDataType::BinDataGeneral)));
    }
    return tagVec;
}

std::unique_ptr<InMatchExpression> FLEQueryRewriter::rewriteEq(
    const EqualityMatchExpression* expr) {
    auto ffp = expr->getData();
    if (!isFleFindPayload(ffp)) {
        return nullptr;
    }

    auto obj = rewritePayloadAsTags(ffp);

    auto tags = std::vector<BSONElement>();
    obj.elems(tags);

    auto inExpr = std::make_unique<InMatchExpression>(kSafeContent);
    inExpr->setBackingBSON(std::move(obj));
    auto status = inExpr->setEqualities(std::move(tags));
    uassertStatusOK(status);

    _rewroteLastExpression = true;
    return inExpr;
}

std::unique_ptr<InMatchExpression> FLEQueryRewriter::rewriteIn(const InMatchExpression* expr) {
    auto backingBSONBuilder = BSONArrayBuilder();
    size_t numFFPs = 0;
    for (auto& eq : expr->getEqualities()) {
        if (isFleFindPayload(eq)) {
            auto obj = rewritePayloadAsTags(eq);
            ++numFFPs;
            for (auto&& elt : obj) {
                backingBSONBuilder.append(elt);
            }
        }
    }
    if (numFFPs == 0) {
        return nullptr;
    }
    // All elements in an encrypted $in expression should be FFPs.
    uassert(
        6329400,
        "If any elements in a $in expression are encrypted, then all elements should be encrypted.",
        numFFPs == expr->getEqualities().size());

    auto backingBSON = backingBSONBuilder.arr();
    auto allTags = std::vector<BSONElement>();
    backingBSON.elems(allTags);

    auto inExpr = std::make_unique<InMatchExpression>(kSafeContent);
    inExpr->setBackingBSON(std::move(backingBSON));
    auto status = inExpr->setEqualities(std::move(allTags));
    uassertStatusOK(status);

    _rewroteLastExpression = true;
    return inExpr;
}

}  // namespace mongo::fle
