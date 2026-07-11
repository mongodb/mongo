// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/query/query_shape/count_cmd_shape.h"
#include "mongo/db/query/query_stats/key.h"
#include "mongo/util/modules.h"

namespace mongo::query_stats {

/**
 * An implementation of the query stats store key for the count command. This class is a simple
 * wrapper around the base 'Key' class and 'CountCmdShape', since the count command doesn't
 * include any command- specific components in its query stats store key beyond the universal
 * components (e.g. hint, read concern).
 */
class CountKey final : public Key {
public:
    CountKey(const boost::intrusive_ptr<ExpressionContext>& expCtx,
             const CountCommandRequest& request,
             std::unique_ptr<query_shape::Shape> countShape,
             query_shape::CollectionType collectionType = query_shape::CollectionType::kUnknown)
        : Key(expCtx->getOperationContext(),
              std::move(countShape),
              request.getHint(),
              request.getReadConcern(),
              request.getMaxTimeMS().has_value(),
              collectionType,
              request.getOriginalQueryShapeHash()) {}

    const SpecificKeyComponents& specificComponents() const override {
        return _components;
    }

private:
    void appendCommandSpecificComponents(
        BSONObjBuilder& bob, const query_shape::SerializationOptions& opts) const final {}

    const EmptyCmdComponents _components{};
};

// This static assert checks to ensure that the struct's size is changed thoughtfully. If adding
// or otherwise changing the members, this assert may be updated with care.
static_assert(sizeof(CountKey) == sizeof(Key) + sizeof(EmptyCmdComponents),
              "If the class' members have changed, this assert may need to be updated with a new "
              "value and the size calcuation will need to be changed.");

}  // namespace mongo::query_stats
