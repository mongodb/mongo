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

#include "mongo/db/query/query_shape/let_shape_component.h"
#include "mongo/db/query/query_shape/query_shape.h"
#include "mongo/util/modules.h"

namespace mongo::query_shape {

/**
 * This struct tracks the components of a find command which are important for the find query shape.
 * It attempts to only track those which are _unique_ to a find command - common elements should go
 * on some super class.
 *
 * Data elements which are shapified like 'filter' are stored in their shapified form. By default
 * and in most cases this will be the representative query shape form so that it can be re-parsed,
 * but as a convenience for serializing it is also supported to construct and serialize this with
 * other options.
 */
struct FindCmdShapeComponents : public CmdSpecificShapeComponents {

    FindCmdShapeComponents(const ParsedFindCommand& request,
                           const boost::intrusive_ptr<ExpressionContext>& expCtx,
                           const SerializationOptions& opts =
                               SerializationOptions::kRepresentativeQueryShapeSerializeOptions);

    /**
     * Appends using the SerializationOptions given in the constructor.
     */
    void appendTo(BSONObjBuilder&,
                  const SerializationOptions&,
                  const boost::intrusive_ptr<ExpressionContext>&) const;

    size_t size() const final {
        return sizeof(FindCmdShapeComponents) + filter.objsize() + projection.objsize() +
            sort.objsize() + min.objsize() + max.objsize() + let.size() - sizeof(LetShapeComponent);
    }

    BSONObj filter;
    BSONObj projection;
    BSONObj sort;
    BSONObj min;
    BSONObj max;

    OptionalBool singleBatch;
    OptionalBool allowDiskUse;
    OptionalBool returnKey;
    OptionalBool showRecordId;
    OptionalBool tailable;
    OptionalBool awaitData;
    OptionalBool mirrored;
    OptionalBool oplogReplay;

    LetShapeComponent let;

    // This anonymous struct represents the presence of the member variables as C++ bit fields.
    // In doing so, each of these boolean values takes up 1 bit instead of 1 byte.
    struct HasField {
        bool projection : 1;
        bool sort : 1;
        bool limit : 1;
        bool skip : 1;
    } hasField;

    // We save a copy of the options used when constructed so we know how to properly append things
    // like limit and skip - either a 1 or "?number". We could have the caller pass the options
    // again during 'appendTo()', but this introduces a risk that the options provided are different
    // than the ones we used to compute 'filter' and the other components.
    SerializationOptions serializationOpts;

    void HashValue(absl::HashState state) const final;

    /**
     * Encodes all optional bools (as well as limit and skip) into a single uint32_t. Every flag
     * takes two bits. 0b00 stands for none, 0b10 for false and 0b11 for true.
     */
    uint32_t optionalArgumentsEncoding() const;
};

class FindCmdShape final : public Shape {
public:
    FindCmdShape(const ParsedFindCommand& findRequest,
                 const boost::intrusive_ptr<ExpressionContext>& expCtx);

    const CmdSpecificShapeComponents& specificComponents() const final;

    /**
     * Assembles a parseable FindCommandRequest representing this shape - some of the pieces are
     * stored right here in the shape, others are in parent classes.
     */
    std::unique_ptr<FindCommandRequest> toFindCommandRequest() const;

    QueryShapeHash sha256Hash(OperationContext*,
                              const SerializationContext& serializationContext) const override;

protected:
    void appendCmdSpecificShapeComponents(BSONObjBuilder&,
                                          OperationContext*,
                                          const SerializationOptions&) const final;

private:
    FindCmdShapeComponents _components;
};

template <typename H>
H AbslHashValue(H h, const FindCmdShapeComponents::HasField& hasField) {
    return H::combine(
        std::move(h), hasField.projection, hasField.sort, hasField.limit, hasField.skip);
}
}  // namespace mongo::query_shape
