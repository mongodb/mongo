/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/query/query_stats/agg_key.h"

#include "mongo/crypto/fle_field_schema_gen.h"
#include "mongo/db/basic_types_gen.h"
#include "mongo/db/database_name.h"
#include "mongo/db/pipeline/exchange_spec_gen.h"
#include "mongo/db/pipeline/external_data_source_option_gen.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/query/explain_options.h"
#include "mongo/db/query/query_shape/agg_cmd_shape.h"
#include "mongo/db/query/query_shape/query_shape.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/db/query/query_shape/shape_helpers.h"
#include "mongo/util/assert_util.h"

#include <functional>
#include <initializer_list>
#include <memory>
#include <numeric>
#include <vector>

#include <absl/container/node_hash_set.h>
#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo::query_stats {

AggCmdComponents::AggCmdComponents(const AggregateCommandRequest& request_,
                                   stdx::unordered_set<NamespaceString> involvedNamespaces_,
                                   const boost::optional<ExplainOptions::Verbosity>& verbosity)
    : involvedNamespaces(std::move(involvedNamespaces_)),
      _bypassDocumentValidation(request_.getBypassDocumentValidation().value_or(false)),
      _verbosity(verbosity),
      _hasField{.batchSize = request_.getCursor().getBatchSize().has_value(),
                .bypassDocumentValidation = request_.getBypassDocumentValidation().has_value(),
                .explain = request_.getExplain().has_value(),
                .passthroughToShard = request_.getPassthroughToShard().has_value()} {}


void AggCmdComponents::HashValue(absl::HashState state) const {
    state = absl::HashState::combine(std::move(state),
                                     _bypassDocumentValidation,
                                     _hasField.batchSize,
                                     _hasField.bypassDocumentValidation,
                                     _verbosity,
                                     _hasField.explain,
                                     _hasField.passthroughToShard);
    // We don't need to add 'involvedNamespaces' here since they are already tracked/duplicated in
    // the Pipeline component of the query shape. We just expose them here for ease of
    // analysis/querying.
}

void AggCmdComponents::appendTo(BSONObjBuilder& bob, const SerializationOptions& opts) const {

    // otherNss
    if (!involvedNamespaces.empty()) {
        BSONArrayBuilder otherNss = bob.subarrayStart(kOtherNssFieldName);
        for (const auto& nss : involvedNamespaces) {
            BSONObjBuilder otherNsEntryBob = otherNss.subobjStart();
            shape_helpers::appendNamespaceShape(otherNsEntryBob, nss, opts);
            otherNsEntryBob.doneFast();
        }
        otherNss.doneFast();
    }

    // bypassDocumentValidation
    if (_hasField.bypassDocumentValidation) {
        bob.append(AggregateCommandRequest::kBypassDocumentValidationFieldName,
                   _bypassDocumentValidation);
    }

    // We don't store the specified batch size values since they don't matter.
    // Provide an arbitrary literal long here.

    tassert(78429,
            "Serialization policy not supported - original values have been discarded",
            !opts.isKeepingLiteralsUnchanged());

    if (_hasField.batchSize) {
        // cursor
        BSONObjBuilder cursorInfo = bob.subobjStart(AggregateCommandRequest::kCursorFieldName);
        opts.appendLiteral(&cursorInfo, SimpleCursorOptions::kBatchSizeFieldName, 0ll);
        cursorInfo.doneFast();
    }

    if (_hasField.explain) {
        // The verbosity can be explicitly set by using the .explain() command, but when using the
        // flag {explain: true} it is set to 'queryPlanner'.
        bob.append(AggregateCommandRequest::kExplainFieldName,
                   ExplainOptions::verbosityString(_verbosity.value()));
    }

    // The values here don't matter (assuming we're not using the 'kUnchanged' policy).
    tassert(8949601,
            "Serialization policy not supported - original values have been discarded",
            !opts.isKeepingLiteralsUnchanged());
    if (_hasField.passthroughToShard) {
        BSONObjBuilder passthroughToShardInfo =
            bob.subobjStart(AggregateCommandRequest::kPassthroughToShardFieldName);
        static const PassthroughToShardOptions representativePassthroughOptions = []() {
            PassthroughToShardOptions passthroughOpts;
            // The value doesn't matter since we will only use this for shapified output.
            passthroughOpts.setShard("?");
            return passthroughOpts;
        }();
        representativePassthroughOptions.serialize(&passthroughToShardInfo, opts);
        passthroughToShardInfo.doneFast();
    }
}

size_t AggCmdComponents::size() const {
    return sizeof(AggCmdComponents) +
        std::accumulate(involvedNamespaces.begin(),
                        involvedNamespaces.end(),
                        0,
                        [](int64_t total, const auto& nss) { return total + nss.size(); });
}

void AggKey::appendCommandSpecificComponents(BSONObjBuilder& bob,
                                             const SerializationOptions& opts) const {
    return _components.appendTo(bob, opts);
}

AggKey::AggKey(const boost::intrusive_ptr<ExpressionContext>& expCtx,
               const AggregateCommandRequest& request,
               std::unique_ptr<query_shape::Shape> aggShape,
               stdx::unordered_set<NamespaceString> involvedNamespaces,
               query_shape::CollectionType collectionType)
    : Key(expCtx->getOperationContext(),
          std::move(aggShape),
          request.getHint(),
          request.getReadConcern(),
          request.getMaxTimeMS().has_value(),
          collectionType),
      _components(request, std::move(involvedNamespaces), expCtx->getExplain()) {}

}  // namespace mongo::query_stats
