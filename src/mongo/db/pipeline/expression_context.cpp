/**
 *    Copyright (C) 2016 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/collation/collator_factory_interface.h"

namespace mongo {

using boost::intrusive_ptr;

ExpressionContext::ResolvedNamespace::ResolvedNamespace(NamespaceString ns,
                                                        std::vector<BSONObj> pipeline)
    : ns(std::move(ns)), pipeline(std::move(pipeline)) {}

ExpressionContext::ExpressionContext(OperationContext* opCtx, const AggregationRequest& request)
    : isExplain(request.isExplain()),
      inShard(request.isFromRouter()),
      extSortAllowed(request.shouldAllowDiskUse()),
      bypassDocumentValidation(request.shouldBypassDocumentValidation()),
      ns(request.getNamespaceString()),
      opCtx(opCtx),
      collation(request.getCollation()) {
    if (!collation.isEmpty()) {
        auto statusWithCollator =
            CollatorFactoryInterface::get(opCtx->getServiceContext())->makeFromBSON(collation);
        uassertStatusOK(statusWithCollator.getStatus());
        setCollator(std::move(statusWithCollator.getValue()));
    }
}

void ExpressionContext::checkForInterrupt() {
    // This check could be expensive, at least in relative terms, so don't check every time.
    if (--interruptCounter == 0) {
        opCtx->checkForInterrupt();
        interruptCounter = kInterruptCheckPeriod;
    }
}

void ExpressionContext::setCollator(std::unique_ptr<CollatorInterface> coll) {
    _collator = std::move(coll);

    // Document/Value comparisons must be aware of the collation.
    _documentComparator = DocumentComparator(_collator.get());
    _valueComparator = ValueComparator(_collator.get());
}

intrusive_ptr<ExpressionContext> ExpressionContext::copyWith(NamespaceString ns) const {
    intrusive_ptr<ExpressionContext> expCtx = new ExpressionContext();

    expCtx->isExplain = isExplain;
    expCtx->inShard = inShard;
    expCtx->inRouter = inRouter;
    expCtx->extSortAllowed = extSortAllowed;
    expCtx->bypassDocumentValidation = bypassDocumentValidation;

    expCtx->ns = std::move(ns);
    expCtx->tempDir = tempDir;

    expCtx->opCtx = opCtx;

    expCtx->collation = collation;
    if (_collator) {
        expCtx->setCollator(_collator->clone());
    }

    expCtx->resolvedNamespaces = resolvedNamespaces;

    // Note that we intentionally skip copying the value of 'interruptCounter' because 'expCtx' is
    // intended to be used for executing a separate aggregation pipeline.

    return expCtx;
}

}  // namespace mongo
