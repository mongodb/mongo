// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/query_shape/let_shape_component.h"
#include "mongo/db/query/query_shape/query_shape.h"
#include "mongo/db/query/write_ops/parsed_find_and_modify.h"
#include "mongo/util/modules.h"

#include <vector>

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo::query_shape {

/**
 * A struct representing the findAndModify command's specific components that are considered part
 * of the query shape.
 *
 * Like the update command shape, the shapified components (e.g. 'representativeQuery') are stored
 * in BSON as a memory optimization, so they can be re-parsed to compute a different shape.
 */
struct FindAndModifyCmdShapeComponents : public CmdSpecificShapeComponents {
    FindAndModifyCmdShapeComponents(
        const write_ops::FindAndModifyCommandRequest& command,
        const ParsedUpdate& parsedUpdate,
        LetShapeComponent let,
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const query_shape::SerializationOptions& opts =
            query_shape::SerializationOptions::kRepresentativeQueryShapeSerializeOptions,
        const projection_ast::Projection* cachedProjection = nullptr);

    FindAndModifyCmdShapeComponents(
        const write_ops::FindAndModifyCommandRequest& command,
        const ParsedDelete& parsedDelete,
        LetShapeComponent let,
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const query_shape::SerializationOptions& opts =
            query_shape::SerializationOptions::kRepresentativeQueryShapeSerializeOptions,
        const projection_ast::Projection* cachedProjection = nullptr);

    size_t size() const final;

    void appendTo(BSONObjBuilder&,
                  const query_shape::SerializationOptions&,
                  const boost::intrusive_ptr<ExpressionContext>&) const;

    void HashValue(absl::HashState state) const final;

    /**
     * Returns the representative value corresponding to the 'update' parameter. It may be an
     * object (modifier or replacement update) or an array (pipeline update); BSONElement can
     * represent both, but does not own its data, so '_representativeUpdateObj' keeps the backing
     * storage alive.
     */
    BSONElement getRepresentativeUpdate() const {
        return _representativeUpdateObj.firstElement();
    }

    // Representative shapes serialized with kRepresentativeQueryShapeSerializeOptions. An empty
    // 'representativeSort'/'representativeFields' means the corresponding command field
    // contributes nothing to the shape: it is absent, empty, or a $natural sort (which is really
    // a hint).
    BSONObj representativeQuery;
    BSONObj representativeSort;
    BSONObj representativeFields;
    BSONObj _representativeUpdateObj;  // Backing BSONObj for the update. Has one element.
    boost::optional<std::vector<BSONObj>> representativeArrayFilters;

    // Which branch of the command was parsed, and the update branch's 'upsert' flag. These are
    // semantic values and must not be recovered from their shapified output: the representative
    // serialization of 'remove: true' is the representative boolean, which would not identify the
    // branch when re-parsing the representative shape.
    bool isRemove;
    bool upsert;

    LetShapeComponent let;
};

/**
 * A class representing the query shape of a findAndModify command.
 */
class FindAndModifyCmdShape : public Shape {
public:
    FindAndModifyCmdShape(const write_ops::FindAndModifyCommandRequest& command,
                          const ParsedUpdate& parsedUpdate,
                          const boost::intrusive_ptr<ExpressionContext>& expCtx);

    FindAndModifyCmdShape(const write_ops::FindAndModifyCommandRequest& command,
                          const ParsedDelete& parsedDelete,
                          const boost::intrusive_ptr<ExpressionContext>& expCtx);

    FindAndModifyCmdShape(const ParsedFindAndModify&,
                          const boost::intrusive_ptr<ExpressionContext>&);

    const CmdSpecificShapeComponents& specificComponents() const final;

    size_t extraSize() const final;

protected:
    void appendCmdSpecificShapeComponents(BSONObjBuilder&,
                                          OperationContext*,
                                          const query_shape::SerializationOptions&) const final;

private:
    FindAndModifyCmdShapeComponents _components;
};

}  // namespace mongo::query_shape
