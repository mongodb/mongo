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
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/crypto/fle_crypto.h"
#include "mongo/crypto/fle_tags.h"
#include "mongo/db/fle_crud.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/transaction_api.h"
#include "mongo/s/grid.h"
#include "mongo/s/transaction_router_resource_yielder.h"
#include "mongo/util/assert_util.h"

namespace mongo::fle {
namespace {
boost::intrusive_ptr<ExpressionContext> makeExpCtx(OperationContext* opCtx,
                                                   FindCommandRequest* findCommand) {
    invariant(findCommand->getNamespaceOrUUID().nss());

    std::unique_ptr<CollatorInterface> collator;
    if (!findCommand->getCollation().isEmpty()) {
        auto statusWithCollator = CollatorFactoryInterface::get(opCtx->getServiceContext())
                                      ->makeFromBSON(findCommand->getCollation());

        uassertStatusOK(statusWithCollator.getStatus());
        collator = std::move(statusWithCollator.getValue());
    }
    return make_intrusive<ExpressionContext>(opCtx,
                                             std::move(collator),
                                             findCommand->getNamespaceOrUUID().nss().get(),
                                             findCommand->getLegacyRuntimeConstants(),
                                             findCommand->getLet());
}
}  // namespace

BSONObj rewriteEncryptedFilter(boost::intrusive_ptr<ExpressionContext> expCtx,
                               const FLEStateCollectionReader& escReader,
                               const FLEStateCollectionReader& eccReader,
                               BSONObj filter) {
    return MatchExpressionRewrite(expCtx, escReader, eccReader, filter).get();
}

void processFindCommand(OperationContext* opCtx,
                        NamespaceString nss,
                        FindCommandRequest* findCommand) {
    invariant(findCommand->getEncryptionInformation());

    auto efc = EncryptionInformationHelpers::getAndValidateSchema(
        nss, findCommand->getEncryptionInformation().get());

    // The transaction runs in a separate executor, and so we can't pass data by
    // reference into the lambda. This struct holds all the data we need inside the
    // lambda, and is passed in a more threadsafe shared_ptr.
    struct SharedBlock {
        SharedBlock(NamespaceString nss,
                    std::string esc,
                    std::string ecc,
                    const BSONObj userFilter,
                    boost::intrusive_ptr<ExpressionContext> expCtx)
            : esc(std::move(esc)),
              ecc(std::move(ecc)),
              userFilter(userFilter),
              db(nss.db()),
              expCtx(expCtx) {}
        std::string esc;
        std::string ecc;
        const BSONObj userFilter;
        BSONObj rewrittenFilter;
        std::string db;
        boost::intrusive_ptr<ExpressionContext> expCtx;
    };

    auto sharedBlock = std::make_shared<SharedBlock>(nss,
                                                     efc.getEscCollection().get().toString(),
                                                     efc.getEccCollection().get().toString(),
                                                     findCommand->getFilter().getOwned(),
                                                     makeExpCtx(opCtx, findCommand));

    auto txn = std::make_shared<txn_api::TransactionWithRetries>(
        opCtx,
        Grid::get(opCtx)->getExecutorPool()->getFixedExecutor(),
        TransactionRouterResourceYielder::make());

    auto swCommitResult = txn->runSyncNoThrow(
        opCtx, [sharedBlock](const txn_api::TransactionClient& txnClient, auto txnExec) {
            auto makeCollectionReader = [sharedBlock](FLEQueryInterface* queryImpl,
                                                      const StringData& coll) {
                NamespaceString nss(sharedBlock->db, coll);
                auto docCount = queryImpl->countDocuments(nss);
                return TxnCollectionReader(docCount, queryImpl, nss);
            };

            // Construct FLE rewriter from the transaction client and encryptionInformation.
            auto queryInterface = FLEQueryInterfaceImpl(txnClient);
            auto escReader = makeCollectionReader(&queryInterface, sharedBlock->esc);
            auto eccReader = makeCollectionReader(&queryInterface, sharedBlock->ecc);

            // Rewrite the MatchExpression.
            sharedBlock->rewrittenFilter = rewriteEncryptedFilter(
                sharedBlock->expCtx, escReader, eccReader, sharedBlock->userFilter);

            return SemiFuture<void>::makeReady();
        });

    uassertStatusOK(swCommitResult);
    uassertStatusOK(swCommitResult.getValue().cmdStatus);
    uassertStatusOK(swCommitResult.getValue().getEffectiveStatus());

    auto rewrittenFilter = sharedBlock->rewrittenFilter.getOwned();
    findCommand->setFilter(std::move(rewrittenFilter));
    findCommand->setEncryptionInformation(boost::none);

    // If we are in a multi-document transaction, then the transaction API has taken
    // care of setting the readConcern on the transaction, and the find command
    // shouldn't provide its own readConcern.
    if (opCtx->inMultiDocumentTransaction()) {
        findCommand->setReadConcern(boost::none);
    }
}

std::unique_ptr<MatchExpression> MatchExpressionRewrite::_rewriteMatchExpression(
    std::unique_ptr<MatchExpression> expr) {
    if (auto result = _rewrite(expr.get())) {
        return result;
    } else {
        return expr;
    }
}

// Rewrite the passed-in match expression in-place.
std::unique_ptr<MatchExpression> MatchExpressionRewrite::_rewrite(MatchExpression* expr) {
    switch (expr->matchType()) {
        case MatchExpression::EQ:
            return rewriteEq(std::move(static_cast<const EqualityMatchExpression*>(expr)));
        case MatchExpression::MATCH_IN:
            return rewriteIn(std::move(static_cast<const InMatchExpression*>(expr)));
        case MatchExpression::AND:
        case MatchExpression::OR:
        case MatchExpression::NOT:
        case MatchExpression::NOR:
            for (size_t i = 0; i < expr->numChildren(); i++) {
                auto child = expr->getChild(i);
                if (auto newChild = _rewrite(child)) {
                    expr->resetChild(i, newChild.release());
                }
            }
            return nullptr;
        default:
            return nullptr;
    }
}

BSONObj MatchExpressionRewrite::rewritePayloadAsTags(BSONElement fleFindPayload) {
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

std::unique_ptr<InMatchExpression> MatchExpressionRewrite::rewriteEq(
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
    return inExpr;
}

std::unique_ptr<InMatchExpression> MatchExpressionRewrite::rewriteIn(
    const InMatchExpression* expr) {
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

    return inExpr;
}

}  // namespace mongo::fle
