// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/query/query_shape/distinct_cmd_shape.h"
#include "mongo/db/query/query_stats/key.h"
#include "mongo/util/modules.h"

#include <memory>


namespace mongo::query_stats {

/**
 * An implementation of the query stats store key for the distinct command. This class is a simple
 * wrapper around the base 'Key' class and 'DistinctCmdShape', since the distinct command doesn't
 * include any command- specific components in its query stats store key beyond the universal
 * components (e.g. hint, read concern).
 */
class DistinctKey final : public Key {
public:
    DistinctKey(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                const DistinctCommandRequest& request,
                std::unique_ptr<query_shape::Shape> distinctShape,
                query_shape::CollectionType collectionType = query_shape::CollectionType::kUnknown)
        : Key(expCtx->getOperationContext(),
              std::move(distinctShape),
              request.getHint(),
              request.getReadConcern(),
              request.getMaxTimeMS().has_value(),
              collectionType,
              request.getOriginalQueryShapeHash()),
          _components() {}

    const SpecificKeyComponents& specificComponents() const override {
        return _components;
    }

private:
    void appendCommandSpecificComponents(
        BSONObjBuilder& bob, const query_shape::SerializationOptions& opts) const final {}

    EmptyCmdComponents _components;
};

// This static assert checks to ensure that the struct's size is changed thoughtfully. If adding
// or otherwise changing the members, this assert may be updated with care.
static_assert(sizeof(DistinctKey) == sizeof(Key) + sizeof(EmptyCmdComponents),
              "If the class' members have changed, this assert may need to be updated with a new "
              "value and the size calcuation will need to be changed.");

}  // namespace mongo::query_stats
