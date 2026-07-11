// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/query/query_shape/let_shape_component.h"
#include "mongo/db/query/query_shape/query_shape.h"
#include "mongo/util/modules.h"

#include <boost/intrusive_ptr.hpp>

namespace mongo::query_shape {

/**
 * A struct representing the aggregate command's specific components that are to be considered part
 * of the query shape.
 *
 * This struct stores the shapified version of the pipeline as a memory optimization. We'll need to
 * store the BSON version in either case, since often the parsed version needs that BSON to survive
 * as backing memory, so we store the representative pipeline shape so that we are able to parse the
 * pipeline again if we need to compute a different shape.
 */
struct AggCmdShapeComponents : public query_shape::CmdSpecificShapeComponents {
    AggCmdShapeComponents(const AggregateCommandRequest&,
                          stdx::unordered_set<NamespaceString> involvedNamespaces,
                          std::vector<BSONObj> shapifiedPipeline,
                          LetShapeComponent let);

    AggCmdShapeComponents(OptionalBool allowDiskUse,
                          stdx::unordered_set<NamespaceString> involvedNamespaces,
                          std::vector<BSONObj> shapifiedPipeline,
                          LetShapeComponent let);

    size_t size() const final;

    void appendTo(BSONObjBuilder&,
                  const query_shape::SerializationOptions&,
                  const boost::intrusive_ptr<ExpressionContext>&) const;

    void HashValue(absl::HashState state) const final;

    OptionalBool allowDiskUse;

    stdx::unordered_set<NamespaceString> involvedNamespaces;

    // The representative query shape of the pipeline.
    std::vector<BSONObj> representativePipeline;

    LetShapeComponent let;
};

/**
 * A class representing the query shape of an aggregate command. The components are listed above.
 * This class knows how to utilize those components to serialize to BSON with any
 * query_shape::SerializationOptions. Mostly this involves correctly setting up an ExpressionContext
 * to re-parse the request if needed.
 */
class AggCmdShape : public Shape {
public:
    AggCmdShape(const AggregateCommandRequest&,
                NamespaceString origNss,
                stdx::unordered_set<NamespaceString> involvedNamespaces,
                const Pipeline&,
                const boost::intrusive_ptr<ExpressionContext>&);

    const CmdSpecificShapeComponents& specificComponents() const final;

    size_t extraSize() const final;

protected:
    void appendCmdSpecificShapeComponents(BSONObjBuilder&,
                                          OperationContext*,
                                          const query_shape::SerializationOptions&) const final;

private:
    AggCmdShapeComponents _components;

    // Flag to denote if the query was run in a router-role context. Needed to rebuild the "dummy"
    // expression context for re-parsing.
    bool _inRouter;
};
static_assert(sizeof(AggCmdShape) <=
                  sizeof(Shape) + sizeof(AggCmdShapeComponents) + 8 /* bool and padding */,
              "If the class' members have changed, this assert and the extraSize() calculation may "
              "need to be updated with a new value.");
}  // namespace mongo::query_shape
