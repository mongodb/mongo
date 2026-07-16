// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/explain_options.h"
#include "mongo/db/query/query_shape/agg_cmd_shape.h"
#include "mongo/db/query/query_stats/key.h"
#include "mongo/db/shard_role/shard_catalog/collection_type.h"
#include "mongo/util/modules.h"

#include <cstdint>
#include <string_view>
#include <utility>

#include <absl/container/node_hash_map.h>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo::query_stats {
using namespace std::literals::string_view_literals;

/**
 * Struct representing the aggregate command's unique arguments which should be included in the
 * query stats key.
 */
struct AggCmdComponents : public SpecificKeyComponents {
    static constexpr std::string_view kOtherNssFieldName = "otherNss"sv;

    AggCmdComponents(const AggregateCommandRequest&,
                     stdx::unordered_set<NamespaceString> involvedNamespaces,
                     const boost::optional<ExplainOptions::Verbosity>& verbosity);

    void HashValue(absl::HashState state) const final;

    void appendTo(BSONObjBuilder& bob, const query_shape::SerializationOptions& opts) const;

    size_t size() const override;

    stdx::unordered_set<NamespaceString> involvedNamespaces;
    bool _bypassDocumentValidation;
    bool _allowPartialResults;
    const boost::optional<mongo::ExplainOptions::Verbosity> _verbosity;

    // This anonymous struct represents the presence of the member variables as C++ bit fields.
    // In doing so, each of these boolean values takes up 1 bit instead of 1 byte.
    struct HasField {
        bool batchSize : 1 = false;
        bool bypassDocumentValidation : 1 = false;
        bool explain : 1 = false;
        bool passthroughToShard : 1 = false;
        bool allowPartialResults : 1 = false;
    } _hasField;
};

/**
 * Handles shapification for AggregateCommandRequests. Requires a pre-parsed pipeline in order to
 * avoid parsing the raw pipeline multiple times, but users should be sure to provide a
 * non-optimized pipeline.
 */
class AggKey final : public Key {
public:
    AggKey(const boost::intrusive_ptr<ExpressionContext>& expCtx,
           const AggregateCommandRequest& request,
           std::unique_ptr<query_shape::Shape> aggShape,
           stdx::unordered_set<NamespaceString> involvedNamespaces,
           query_shape::CollectionType collectionType = query_shape::CollectionType::kUnknown);

    const SpecificKeyComponents& specificComponents() const final {
        return _components;
    }

protected:
    void appendCommandSpecificComponents(BSONObjBuilder& bob,
                                         const query_shape::SerializationOptions& opts) const final;

private:
    const AggCmdComponents _components;
};
static_assert(
    sizeof(AggKey) == sizeof(Key) + sizeof(AggCmdComponents),
    "If the class' members have changed, this assert may need to be updated with a new value.");
}  // namespace mongo::query_stats
