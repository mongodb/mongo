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

#include "mongo/db/query/parsed_distinct_command.h"
#include "mongo/db/query/query_shape/query_shape.h"

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

    void appendCmdSpecificShapeComponents(BSONObjBuilder&,
                                          OperationContext*,
                                          const SerializationOptions& opts) const final;

    QueryShapeHash sha256Hash(OperationContext*,
                              const SerializationContext& serializationContext) const override;

    DistinctCmdShapeComponents components;
};

static_assert(sizeof(DistinctCmdShape) == sizeof(Shape) + sizeof(DistinctCmdShapeComponents),
              "If the class' members have changed, this assert and the extraSize() calculation may "
              "need to be updated with a new value.");

}  // namespace mongo::query_shape
