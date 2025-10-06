/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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
    UpdateCmdShapeComponents(const ParsedUpdate& parsedUpdate,
                             LetShapeComponent let,
                             const SerializationOptions& opts =
                                 SerializationOptions::kRepresentativeQueryShapeSerializeOptions);

    size_t size() const final;

    void appendTo(BSONObjBuilder&,
                  const SerializationOptions&,
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
 * A class representing the query shape of an aggregate command. The components are listed above.
 * This class knows how to utilize those components to serialize to BSON with any
 * SerializationOptions. Mostly this involves correctly setting up an ExpressionContext to re-parse
 * the request if needed.
 */
class UpdateCmdShape : public Shape {
public:
    UpdateCmdShape(const write_ops::UpdateCommandRequest&,
                   const ParsedUpdate&,
                   const boost::intrusive_ptr<ExpressionContext>&);

    const CmdSpecificShapeComponents& specificComponents() const final;

    size_t extraSize() const final;

protected:
    void appendCmdSpecificShapeComponents(BSONObjBuilder&,
                                          OperationContext*,
                                          const SerializationOptions&) const final;

private:
    UpdateCmdShapeComponents _components;
};
static_assert(sizeof(UpdateCmdShape) == sizeof(Shape) + sizeof(UpdateCmdShapeComponents),
              "If the class' members have changed, this assert and the extraSize() calculation may "
              "need to be updated with a new value.");
}  // namespace mongo::query_shape
