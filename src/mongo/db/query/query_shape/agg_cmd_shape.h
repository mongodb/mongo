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

#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/query/query_shape/let_shape_component.h"
#include "mongo/db/query/query_shape/query_shape.h"

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
                  const SerializationOptions&,
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
 * SerializationOptions. Mostly this involves correctly setting up an ExpressionContext to re-parse
 * the request if needed.
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
                                          const SerializationOptions&) const final;

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
