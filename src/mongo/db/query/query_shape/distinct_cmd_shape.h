// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/query/parsed_distinct_command.h"
#include "mongo/db/query/query_shape/query_shape.h"
#include "mongo/util/modules.h"

namespace mongo::query_shape {

/**
 * This struct tracks the components of a distinct command which are important for the distinct
 * query shape. It attempts to only track those which are _unique_ to a distinct command - common
 * elements should go on some super class.
 *
 * Data elements which are shapified like 'query' are stored in their shapified form. By default
 * and in most cases this will be the representative query shape form so that it can be re-parsed,
 * but as a convenience for serializing it is also supported to construct and serialize this with
 * other options.
 */
struct DistinctCmdShapeComponents : public CmdSpecificShapeComponents {

    DistinctCmdShapeComponents(const ParsedDistinctCommand& request,
                               const boost::intrusive_ptr<ExpressionContext>& expCtx);

    void HashValue(absl::HashState state) const final;

    size_t size() const final;

    std::string key;
    BSONObj representativeQuery;
};

class DistinctCmdShape final : public Shape {
public:
    DistinctCmdShape(const ParsedDistinctCommand& distinct,
                     const boost::intrusive_ptr<ExpressionContext>& expCtx);


    const CmdSpecificShapeComponents& specificComponents() const final;

    void appendCmdSpecificShapeComponents(
        BSONObjBuilder&,
        OperationContext*,
        const query_shape::SerializationOptions& opts) const final;

    QueryShapeHash sha256Hash(OperationContext*,
                              const SerializationContext& serializationContext) const override;

    DistinctCmdShapeComponents components;
};

static_assert(sizeof(DistinctCmdShape) == sizeof(Shape) + sizeof(DistinctCmdShapeComponents),
              "If the class' members have changed, this assert and the extraSize() calculation may "
              "need to be updated with a new value.");

}  // namespace mongo::query_shape
