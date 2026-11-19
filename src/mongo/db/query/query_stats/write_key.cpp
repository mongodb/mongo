// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/query_stats/write_key.h"

#include "mongo/db/query/query_feature_flags_gen.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/db/server_options.h"
#include "mongo/util/overloaded_visitor.h"

namespace mongo::query_stats {

template <typename Request>
WriteCmdComponents<Request>::WriteCmdComponents(const Request& request)
    : _ordered(request.getOrdered()),
      _bypassDocumentValidation(request.getBypassDocumentValidation()) {}

template <typename Request>
void WriteCmdComponents<Request>::HashValue(absl::HashState state) const {
    state = absl::HashState::combine(std::move(state), _ordered, _bypassDocumentValidation);
}

template <typename Request>
void WriteCmdComponents<Request>::appendTo(BSONObjBuilder& bob,
                                           const query_shape::SerializationOptions& opts) const {
    bob.append(Request::kOrderedFieldName, _ordered);
    bob.append(Request::kBypassDocumentValidationFieldName, _bypassDocumentValidation);
}

template <typename Request>
size_t WriteCmdComponents<Request>::size() const {
    return sizeof(WriteCmdComponents<Request>);
}

template <typename Request>
void WriteKey<Request>::appendCommandSpecificComponents(
    BSONObjBuilder& bob, const query_shape::SerializationOptions& opts) const {
    _components.appendTo(bob, opts);
}

/**
 * Constructor for insert
 */
template <typename Request>
WriteKey<Request>::WriteKey(OperationContext* opCtx,
                            const write_ops::InsertCommandRequest& request,
                            std::unique_ptr<query_shape::Shape> insertShape,
                            query_shape::CollectionType collectionType)
requires std::same_as<Request, write_ops::InsertCommandRequest>
    : Key(opCtx,
          std::move(insertShape),
          boost::none,  // no hint for insert
          boost::none,  // no readConcern for insert
          request.getMaxTimeMS().has_value(),
          collectionType),
      _components(request) {}

/**
 * Constructor for update and delete
 */
template <typename Request>
WriteKey<Request>::WriteKey(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                            const Request& request,
                            const boost::optional<BSONObj>& hint,
                            std::unique_ptr<query_shape::Shape> shape,
                            query_shape::CollectionType collectionType)
    : Key(expCtx->getOperationContext(),
          std::move(shape),
          hint,
          request.getReadConcern(),
          request.getMaxTimeMS().has_value(),
          collectionType),
      _components(request) {}

// Explicit instantiations for supported write types
template struct WriteCmdComponents<write_ops::UpdateCommandRequest>;
template struct WriteCmdComponents<write_ops::InsertCommandRequest>;
template struct WriteCmdComponents<write_ops::DeleteCommandRequest>;

template class WriteKey<write_ops::UpdateCommandRequest>;
template class WriteKey<write_ops::InsertCommandRequest>;
template class WriteKey<write_ops::DeleteCommandRequest>;

template struct WriteCmdComponents<BulkWriteCommandRequest>;

BulkWriteCmdComponents::BulkWriteCmdComponents(const BulkWriteCommandRequest& request)
    : _writeCmdComponents(request),
      _errorsOnly(request.getErrorsOnly()),
      _hasField{.batchSize =
                    request.getCursor() && request.getCursor()->getBatchSize().has_value()} {}

void BulkWriteCmdComponents::HashValue(absl::HashState state) const {
    state = absl::HashState::combine(std::move(state), _writeCmdComponents, _errorsOnly, _hasField);
}

void BulkWriteCmdComponents::appendTo(BSONObjBuilder& bob,
                                      const query_shape::SerializationOptions& opts) const {
    tassert(13743400,
            "Serialization policy not supported - original values have been discarded",
            !opts.isKeepingLiteralsUnchanged());

    _writeCmdComponents.appendTo(bob, opts);
    bob.append(BulkWriteCommandRequest::kErrorsOnlyFieldName, _errorsOnly);

    if (_hasField.batchSize) {
        BSONObjBuilder cursorInfo = bob.subobjStart(BulkWriteCommandRequest::kCursorFieldName);
        opts.appendLiteral(&cursorInfo, SimpleCursorOptions::kBatchSizeFieldName, 0ll);
        cursorInfo.doneFast();
    }
}

size_t BulkWriteCmdComponents::size() const {
    return sizeof(BulkWriteCmdComponents);
}

namespace {
const auto& getBulkWriteOp(const BulkWriteCommandRequest& request, size_t opIdx) {
    tassert(13743401, "bulkWrite op index is out of range", opIdx < request.getOps().size());
    return request.getOps()[opIdx];
}

boost::optional<BSONObj> getBulkWriteHint(const BulkWriteCommandRequest& request, size_t opIdx) {
    return visit(
        OverloadedVisitor{
            [](const BulkWriteInsertOp&) -> boost::optional<BSONObj> { return boost::none; },
            [](const BulkWriteUpdateOp& op) -> boost::optional<BSONObj> { return op.getHint(); },
            [](const BulkWriteDeleteOp& op) -> boost::optional<BSONObj> {
                return op.getHint();
            }},
        getBulkWriteOp(request, opIdx));
}

bool isBulkWriteInsertOp(const BulkWriteCommandRequest& request, size_t opIdx) {
    return holds_alternative<BulkWriteInsertOp>(getBulkWriteOp(request, opIdx));
}
}  // namespace

BulkWriteKey::BulkWriteKey(OperationContext* opCtx,
                           const BulkWriteCommandRequest& request,
                           size_t opIdx,
                           std::unique_ptr<query_shape::Shape> shape,
                           query_shape::CollectionType collectionType)
    : Key(opCtx,
          std::move(shape),
          getBulkWriteHint(request, opIdx),
          isBulkWriteInsertOp(request, opIdx) ? boost::none : request.getReadConcern(),
          request.getMaxTimeMS().has_value(),
          collectionType),
      _components(request) {}

void BulkWriteKey::appendCommandSpecificComponents(
    BSONObjBuilder& bob, const query_shape::SerializationOptions& opts) const {
    _components.appendTo(bob, opts);
}

bool shouldComputeBulkWriteQueryShape(OperationContext* opCtx,
                                      const BulkWriteCommandRequest& request,
                                      size_t opIdx,
                                      query_shape::CollectionType collectionType) {
    if (!feature_flags::gFeatureFlagQueryStatsAdditionalWriteCommands
             .isEnabledUseLastLTSFCVWhenUninitialized(
                 VersionContext::getDecoration(opCtx),
                 serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
        return false;
    }

    if (collectionType == query_shape::CollectionType::kTimeseries) {
        return false;
    }

    const auto& op = getBulkWriteOp(request, opIdx);
    const auto& nsEntry =
        request.getNsInfo()[visit([](const auto& op) -> size_t { return op.getNsInfoIdx(); }, op)];
    // Encrypted fields would alter the query shape, and timeseries ops are rewritten against the
    // buckets collection.
    if (nsEntry.getEncryptionInformation() || nsEntry.getIsTimeseriesNamespace()) {
        return false;
    }

    if (const auto* updateOp = get_if<BulkWriteUpdateOp>(&op)) {
        return isQueryShapeSupportedUpdateType(updateOp->getUpdateMods().type());
    }

    return true;
}

bool isQueryShapeSupportedUpdateType(write_ops::UpdateModification::Type type) {
    switch (type) {
        case write_ops::UpdateModification::Type::kReplacement:
        case write_ops::UpdateModification::Type::kModifier:
        case write_ops::UpdateModification::Type::kPipeline:
            return true;
        default:
            return false;
    }
}

}  // namespace mongo::query_stats
