// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/parsed_find_command.h"
#include "mongo/db/query/query_shape/query_shape.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/util/modules.h"

namespace mongo::query_shape {

/**
 * The struct stores the shapified version of the let parameter.
 */
struct LetShapeComponent : public CmdSpecificShapeComponents {
    LetShapeComponent(boost::optional<BSONObj> let,
                      const boost::intrusive_ptr<ExpressionContext>& expCtx);

    void HashValue(absl::HashState state) const final;
    size_t size() const final;

    /**
     * Appends the shapified let parameter to the builder.
     */
    void appendTo(BSONObjBuilder&,
                  const query_shape::SerializationOptions&,
                  const boost::intrusive_ptr<ExpressionContext>&) const;

    BSONObj shapifiedLet;
    bool hasLet;
};
}  // namespace mongo::query_shape
