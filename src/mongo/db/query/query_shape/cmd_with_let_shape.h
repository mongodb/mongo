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

#include "mongo/bson/bsonobj.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/parsed_find_command.h"
#include "mongo/db/query/query_shape/query_shape.h"
#include "mongo/db/query/query_shape/serialization_options.h"

namespace mongo::query_shape {

/**
 * This struct is bit of a weird one. We want to use it as the shape's _entire_ "specific
 * components" (rather than introduce more virtual functions to that interface). So, we track here
 * the let component (as the name suggests) but we also keep an unowned reference to the specific
 * components of CmdWithLetShape sub-classes. This class doesn't really do all that much with those
 * components except track a reference to them and ensure their size is accounted for and their hash
 * value is incorporated.
 */
struct LetShapeComponent : public CmdSpecificShapeComponents {
    LetShapeComponent(boost::optional<BSONObj> let,
                      const boost::intrusive_ptr<ExpressionContext>& expCtx,
                      const CmdSpecificShapeComponents& unownedInnerComponents);

    /**
     * Hashes to include the shapified let parameters and also the hash of 'unownedInnerComponents'.
     */
    void HashValue(absl::HashState state) const final;

    /**
     * Includes the size of the let parameters and the size of 'unownedInnerComponents.'
     */
    size_t size() const final;

    /**
     * Adds _only_ the let params.
     */
    void addLetBson(BSONObjBuilder&,
                    const SerializationOptions&,
                    const boost::intrusive_ptr<ExpressionContext>&) const;

    BSONObj shapifiedLet;
    bool hasLet;
    // Tracked so that this can be hash combined correctly.
    const CmdSpecificShapeComponents& unownedInnerComponents;
};

/**
 * The 'let' command argument is semi-generic in that it is supported in a couple commands. However
 * it is treated specially since it supports using expressions as the let constants. Using
 * expressions induces a library dependency that we don't want in the Shape interface itself. So
 * this class handles tracking and adding the 'let' component of the shape for sub-classes.
 */
class CmdWithLetShape : public Shape {
public:
    CmdWithLetShape(boost::optional<BSONObj> let,
                    const boost::intrusive_ptr<ExpressionContext>& expCtx,
                    const CmdSpecificShapeComponents& unownedInnerComponents,
                    NamespaceStringOrUUID,
                    BSONObj collation_);

    const CmdSpecificShapeComponents& specificComponents() const final {
        return _let;
    }

protected:
    void appendCmdSpecificShapeComponents(BSONObjBuilder&,
                                          OperationContext* opCtx,
                                          const SerializationOptions& opts) const final;
    virtual void appendLetCmdSpecificShapeComponents(
        BSONObjBuilder&,
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const SerializationOptions&) const = 0;

    LetShapeComponent _let;
};
static_assert(sizeof(CmdWithLetShape) == sizeof(Shape) + sizeof(LetShapeComponent),
              "If the class' members have changed, this assert and the extraSize() calculation may "
              "need to be updated with a new value.");

}  // namespace mongo::query_shape
