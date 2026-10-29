// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/query_shape/let_shape_component.h"
#include "mongo/db/query/query_shape/query_shape.h"
#include "mongo/db/query/write_ops/parsed_update.h"
#include "mongo/db/query/write_ops/write_ops_gen.h"
#include "mongo/util/modules.h"

#include <boost/intrusive_ptr.hpp>

namespace mongo::query_shape {

/**
 * A struct representing the update command's specific components that are to be considered part
 * of the query shape.
 *
 * This struct stores the shapified compoments (e.g., 'representativeQ') in BSON as a memory
 * optimization. We choose to store them in BSON rather than in their parsed objects (e.g.,
 * ParsedUpdate) because those parsed versions often still need these BSONs to survive as backing
 * memory. So we store the representative components so that we are able to parse those components
 * again if we need to compute a different shape.
 */
struct UpdateCmdShapeComponents : public query_shape::CmdSpecificShapeComponents {
    UpdateCmdShapeComponents(
        const ParsedUpdate& parsedUpdate,
        LetShapeComponent let,
        const query_shape::SerializationOptions& opts =
            query_shape::SerializationOptions::kRepresentativeQueryShapeSerializeOptions);

    size_t size() const final;

    void appendTo(BSONObjBuilder&,
                  const query_shape::SerializationOptions&,
                  const boost::intrusive_ptr<ExpressionContext>&) const;

    void HashValue(absl::HashState state) const final;

    /**
     * Returns the representative value corresponding to the 'u' (update) parameter to the update
     * statement.
     *
     * Since u could be of various types. For example, it may be a BSONObj for a modification update
     * or a BSONArray for a pipeline update, we use BSONElement to represent 'u' because it is
     * capable of representing various types. But BSONElement does not own the data itself, we need
     * to keep the backing BSONObj '_representativeUObj'.
     */
    inline BSONElement getRepresentativeU() const {
        return _representativeUObj.firstElement();
    }

    // Representative shapes serialized with kRepresentativeQueryShapeSerializeOptions.
    BSONObj representativeQ;
    BSONObj _representativeUObj;  // The backing BSONObj for u. It always only has one element.
    boost::optional<BSONObj> representativeC;
    boost::optional<std::vector<BSONObj>> representativeArrayFilters;

    bool multi;
    bool upsert;

    LetShapeComponent let;
};

/**
 * A class representing the query shape of an update command. The components are listed above.
 * This class knows how to utilize those components to serialize to BSON with any
 * query_shape::SerializationOptions. Mostly this involves correctly setting up an ExpressionContext
 * to re-parse the request if needed.
 */
class UpdateCmdShape : public Shape {
public:
    UpdateCmdShape(const write_ops::UpdateCommandRequest&,
                   const ParsedUpdate&,
                   const boost::intrusive_ptr<ExpressionContext>&);

    const CmdSpecificShapeComponents& specificComponents() const final;

    size_t extraSize() const final;

    QueryShapeHash sha256Hash(OperationContext*,
                              const SerializationContext& serializationContext) const override;

protected:
    void appendCmdSpecificShapeComponents(BSONObjBuilder&,
                                          OperationContext*,
                                          const query_shape::SerializationOptions&) const final;

private:
    UpdateCmdShapeComponents _components;
};
static_assert(sizeof(UpdateCmdShape) == sizeof(Shape) + sizeof(UpdateCmdShapeComponents),
              "If the class' members have changed, this assert and the extraSize() calculation may "
              "need to be updated with a new value.");
}  // namespace mongo::query_shape
