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

#pragma once

#include <cstdint>
#include <utility>

#include <absl/container/node_hash_map.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/collection_type.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/query_shape/query_shape.h"
#include "mongo/db/query/query_stats/key_generator.h"

namespace mongo::query_stats {

/**
 * Struct representing the aggregate command's unique arguments which should be included in the
 * query stats key.
 */
struct AggCmdComponents : public SpecificKeyComponents {
    static constexpr StringData kOtherNssFieldName = "otherNss"_sd;

    AggCmdComponents(const AggregateCommandRequest&,
                     stdx::unordered_set<NamespaceString> involvedNamespaces);

    void HashValue(absl::HashState state) const final;

    void appendTo(BSONObjBuilder& bob, const SerializationOptions& opts) const;

    int64_t size() const;

    // TODO SERVER-76330 owned here for now, duplicating memory... This is not really accounted for
    // right now but we should fix it soon.
    AggregateCommandRequest request;
    stdx::unordered_set<NamespaceString> involvedNamespaces;
};

/**
 * Handles shapification for AggregateCommandRequests. Requires a pre-parsed pipeline in order to
 * avoid parsing the raw pipeline multiple times, but users should be sure to provide a
 * non-optimized pipeline.
 */
class AggKeyGenerator final : public KeyGenerator {
public:
    AggKeyGenerator(
        AggregateCommandRequest request,
        const Pipeline& pipeline,
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        stdx::unordered_set<NamespaceString> involvedNamespaces,
        const NamespaceString& origNss,
        query_shape::CollectionType collectionType = query_shape::CollectionType::kUnknown);

    const SpecificKeyComponents& specificComponents() const final {
        return _components;
    }

    // The default implementation of hashing for smart pointers is not a good one for our purposes.
    // Here we overload them to actually take the hash of the object, rather than hashing the
    // pointer itself.
    template <typename H>
    friend H AbslHashValue(H h, const std::unique_ptr<const AggKeyGenerator>& keyGenerator) {
        return H::combine(std::move(h), *keyGenerator);
    }
    template <typename H>
    friend H AbslHashValue(H h, const std::shared_ptr<const AggKeyGenerator>& keyGenerator) {
        return H::combine(std::move(h), *keyGenerator);
    }


protected:
    void appendCommandSpecificComponents(BSONObjBuilder& bob,
                                         const SerializationOptions& opts) const final override;

private:
    const AggCmdComponents _components;
};
}  // namespace mongo::query_stats
