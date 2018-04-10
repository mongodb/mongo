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
#include "mongo/db/pipeline/stub_mongo_process_interface.h"
#include "mongo/db/query/collation/collation_spec.h"
#include "mongo/db/query/collation/collator_factory_interface.h"

namespace mongo {

using boost::intrusive_ptr;

ExpressionContext::ResolvedNamespace::ResolvedNamespace(NamespaceString ns,
                                                        std::vector<BSONObj> pipeline)
    : ns(std::move(ns)), pipeline(std::move(pipeline)) {}

ExpressionContext::ExpressionContext(OperationContext* opCtx,
                                     const AggregationRequest& request,
                                     std::unique_ptr<CollatorInterface> collator,
                                     std::shared_ptr<MongoProcessInterface> processInterface,
                                     StringMap<ResolvedNamespace> resolvedNamespaces,
                                     boost::optional<UUID> collUUID)
    : ExpressionContext(opCtx, collator.get()) {
    explain = request.getExplain();
    comment = request.getComment();
    fromMongos = request.isFromMongos();
    needsMerge = request.needsMerge();
    allowDiskUse = request.shouldAllowDiskUse();
    bypassDocumentValidation = request.shouldBypassDocumentValidation();
    ns = request.getNamespaceString();
    mongoProcessInterface = std::move(processInterface);
    collation = request.getCollation();
    _ownedCollator = std::move(collator);
    _resolvedNamespaces = std::move(resolvedNamespaces);
    uuid = std::move(collUUID);
}

ExpressionContext::ExpressionContext(OperationContext* opCtx, const CollatorInterface* collator)
    : opCtx(opCtx),
      mongoProcessInterface(std::make_shared<StubMongoProcessInterface>()),
      timeZoneDatabase(opCtx && opCtx->getServiceContext()
                           ? TimeZoneDatabase::get(opCtx->getServiceContext())
                           : nullptr),
      variablesParseState(variables.useIdGenerator()),
      _collator(collator),
      _documentComparator(_collator),
      _valueComparator(_collator) {}

ExpressionContext::ExpressionContext(NamespaceString nss,
                                     std::shared_ptr<MongoProcessInterface> processInterface,
                                     const TimeZoneDatabase* tzDb)
    : ns(std::move(nss)),
      mongoProcessInterface(std::move(processInterface)),
      timeZoneDatabase(tzDb),
      variablesParseState(variables.useIdGenerator()) {}

void ExpressionContext::checkForInterrupt() {
    // This check could be expensive, at least in relative terms, so don't check every time.
    if (--_interruptCounter == 0) {
        invariant(opCtx);
        _interruptCounter = kInterruptCheckPeriod;
        opCtx->checkForInterrupt();
    }
}

ExpressionContext::CollatorStash::CollatorStash(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    std::unique_ptr<CollatorInterface> newCollator)
    : _expCtx(expCtx),
      _originalCollation(_expCtx->collation),
      _originalCollatorOwned(std::move(_expCtx->_ownedCollator)),
      _originalCollatorUnowned(_expCtx->_collator) {
    _expCtx->setCollator(std::move(newCollator));
    _expCtx->collation =
        _expCtx->getCollator() ? _expCtx->getCollator()->getSpec().toBSON().getOwned() : BSONObj();
}

ExpressionContext::CollatorStash::~CollatorStash() {
    if (_originalCollatorOwned) {
        _expCtx->setCollator(std::move(_originalCollatorOwned));
    } else {
        _expCtx->setCollator(_originalCollatorUnowned);
        if (!_originalCollatorUnowned && _expCtx->_ownedCollator) {
            // If the original collation was 'nullptr', we cannot distinguish whether it was owned
            // or not. We always set '_ownedCollator' with the stash, so should reset it to null
            // here.
            _expCtx->_ownedCollator = nullptr;
        }
    }
    _expCtx->collation = _originalCollation;
}

std::unique_ptr<ExpressionContext::CollatorStash> ExpressionContext::temporarilyChangeCollator(
    std::unique_ptr<CollatorInterface> newCollator) {
    // This constructor of CollatorStash is private, so we can't use make_unique().
    return std::unique_ptr<CollatorStash>(new CollatorStash(this, std::move(newCollator)));
}

void ExpressionContext::setCollator(const CollatorInterface* collator) {
    _collator = collator;

    // Document/Value comparisons must be aware of the collation.
    _documentComparator = DocumentComparator(_collator);
    _valueComparator = ValueComparator(_collator);
}

intrusive_ptr<ExpressionContext> ExpressionContext::copyWith(
    NamespaceString ns,
    boost::optional<UUID> uuid,
    boost::optional<std::unique_ptr<CollatorInterface>> collator) const {
    intrusive_ptr<ExpressionContext> expCtx =
        new ExpressionContext(std::move(ns), mongoProcessInterface, timeZoneDatabase);

    expCtx->uuid = std::move(uuid);
    expCtx->explain = explain;
    expCtx->comment = comment;
    expCtx->needsMerge = needsMerge;
    expCtx->fromMongos = fromMongos;
    expCtx->inMongos = inMongos;
    expCtx->allowDiskUse = allowDiskUse;
    expCtx->bypassDocumentValidation = bypassDocumentValidation;
    expCtx->subPipelineDepth = subPipelineDepth;

    expCtx->tempDir = tempDir;

    expCtx->opCtx = opCtx;

    if (collator) {
        expCtx->collation =
            *collator ? (*collator)->getSpec().toBSON() : CollationSpec::kSimpleSpec;
        expCtx->setCollator(std::move(*collator));
    } else {
        expCtx->collation = collation;
        if (_ownedCollator) {
            expCtx->setCollator(_ownedCollator->clone());
        } else if (_collator) {
            expCtx->setCollator(_collator);
        }
    }

    expCtx->_resolvedNamespaces = _resolvedNamespaces;

    // Note that we intentionally skip copying the value of '_interruptCounter' because 'expCtx' is
    // intended to be used for executing a separate aggregation pipeline.

    return expCtx;
}

}  // namespace mongo
