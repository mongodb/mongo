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

#include "mongo/db/query/query_stats/agg_key_generator.h"

#include <absl/container/node_hash_set.h>
#include <boost/cstdint.hpp>
#include <functional>
#include <initializer_list>
#include <memory>
#include <numeric>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

#include "mongo/crypto/fle_field_schema_gen.h"
#include "mongo/db/basic_types_gen.h"
#include "mongo/db/database_name.h"
#include "mongo/db/pipeline/exchange_spec_gen.h"
#include "mongo/db/pipeline/external_data_source_option_gen.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/query/query_shape/agg_cmd_shape.h"
#include "mongo/db/query/query_shape/query_shape.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/db/query/query_shape/shape_helpers.h"
#include "mongo/util/assert_util.h"

namespace mongo::query_stats {

AggCmdComponents::AggCmdComponents(const AggregateCommandRequest& request_,
                                   stdx::unordered_set<NamespaceString> involvedNamespaces_)
    : request(request_), involvedNamespaces(std::move(involvedNamespaces_)) {}

void AggCmdComponents::HashValue(absl::HashState state) const {
    state = absl::HashState::combine(std::move(state),
                                     request.getCursor().getBatchSize().has_value(),
                                     request.getMaxTimeMS().has_value(),
                                     request.getBypassDocumentValidation().has_value());
    // We don't need to add 'involvedNamespaces' here since they are already tracked/duplicated in
    // the Pipeline component of the query shape. We just expose them here for ease of
    // analysis/querying.
}

void AggCmdComponents::appendTo(BSONObjBuilder& bob, const SerializationOptions& opts) const {
    // cursor
    if (auto param = request.getCursor().getBatchSize()) {
        BSONObjBuilder cursorInfo = bob.subobjStart(AggregateCommandRequest::kCursorFieldName);
        opts.appendLiteral(&cursorInfo,
                           SimpleCursorOptions::kBatchSizeFieldName,
                           static_cast<long long>(param.get()));
        cursorInfo.doneFast();
    }

    // maxTimeMS
    if (auto param = request.getMaxTimeMS()) {
        opts.appendLiteral(&bob,
                           AggregateCommandRequest::kMaxTimeMSFieldName,
                           static_cast<long long>(param.get()));
    }

    // bypassDocumentValidation
    if (auto param = request.getBypassDocumentValidation()) {
        opts.appendLiteral(
            &bob, AggregateCommandRequest::kBypassDocumentValidationFieldName, bool(param.get()));
    }

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
}

int64_t AggCmdComponents::size() const {
    // TODO SERVER-76330 we ignore the size of request here because it is owned by the query shape
    // on the universal components.
    return std::accumulate(involvedNamespaces.begin(),
                           involvedNamespaces.end(),
                           0,
                           [](int64_t total, const auto& nss) { return total + nss.size(); });
}

void AggKeyGenerator::appendCommandSpecificComponents(BSONObjBuilder& bob,
                                                      const SerializationOptions& opts) const {
    return _components.appendTo(bob, opts);
}

AggKeyGenerator::AggKeyGenerator(AggregateCommandRequest request,
                                 const Pipeline& pipeline,
                                 const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                 stdx::unordered_set<NamespaceString> involvedNamespaces,
                                 const NamespaceString& origNss,
                                 query_shape::CollectionType collectionType)
    : KeyGenerator(expCtx->opCtx,
                   std::make_unique<query_shape::AggCmdShape>(
                       request, origNss, involvedNamespaces, pipeline, expCtx),
                   request.getHint(),
                   collectionType),
      _components(request, std::move(involvedNamespaces)) {}

}  // namespace mongo::query_stats
