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

#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <functional>
#include <string>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/crypto/sha256_block.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/db/query/query_shape/shape_helpers.h"

namespace mongo::query_shape {

/**
 * Each type of "query" command likely has different fields/options that are considered important
 * for the shape. For example, a find command has a skip and a limit, and an aggregate command has a
 * pipeline. This interface is used to allow different sub-commands to diverge in this way but still
 * ensure we can appropriately hash them to compare their shapes, and properly account for their
 * size.
 *
 * This struct is split out as a separate inheritence hierarchy from 'Shape' to make it easier to
 * ensure each piece is hashed without sub-classes needing to enumerate the parent class's member
 * variables.
 */
struct CmdSpecificShapeComponents {
    virtual ~CmdSpecificShapeComponents() {}

    /**
     * Sub-classes should implement this in a way which includes all shape-relevant state. If two
     * shapes should compare equal, they should result in the same hash value. For example for the
     * find command - we would include the _shapified_ filter and projection here, but we will not
     * include the comment - which is not part of the shape.
     */
    virtual void HashValue(absl::HashState state) const = 0;

    /**
     * It is important for shape components to accurately report their size, and to make a
     * reasonable effort to maintain a minimal size. We use the query shape in memory-constrained
     * data structures, so a bigger shape means we can have fewer different shapes stored (for
     * example in the query stats store).
     *
     * We cannot just use sizeof() because there are some variable size data members (like BSON
     * objects) which depend on the particular instance.
     */
    virtual size_t size() const = 0;

    // Some template boilerplate to allow sub-classes to overload the hash implementation.
    template <typename H>
    friend H AbslHashValue(H state, const CmdSpecificShapeComponents& value) {
        value.HashValue(absl::HashState::Create(&state));
        return std::move(state);
    }
};

using QueryShapeHash = SHA256Block;

/**
 * A query "shape" is a version of a command with literal values abstracted so that two instances of
 * the command may compare/hash equal even if they use slightly different literal values. This
 * concept exists not just the find command, but planned for many of the CRUD commands + aggregate.
 * It also includes most (but not all) components of these commands, not just the query predicate
 * (MatchExpresssion). In these ways, "query" is meant more generally.
 *
 * A "Query Shape" can vary depending on the command (e.g. find, aggregate, or distinct). This
 * abstract struct is the API we must implement for each command which we want to have a "shape"
 * concept.
 *
 * In order to properly account for the size of a query shape, the CmdSpecificShapeComponents should
 * include all meaningful memory consumption, and be sure to report it in 'size()'. Subclasses of
 * 'Shape' are not expected to have any meaningful memory usage outside of that struct.
 */
class Shape {
public:
    virtual ~Shape() {}

    /**
     * Sub-classes are expected to implement this as a mechanism for plugging in their command
     * specific shape components.
     */
    virtual const CmdSpecificShapeComponents& specificComponents() const = 0;

    /**
     * Note this may involve re-parsing command BSON and so is not necessarily cheap.
     */
    BSONObj toBson(OperationContext*,
                   const SerializationOptions&,
                   const SerializationContext& serializationContext) const;

    /**
     * The Query Shape Hash is defined to be the SHA256 Hash of the representatice query shape. This
     * helper computes that.
     */
    virtual QueryShapeHash sha256Hash(OperationContext*,
                                      const SerializationContext& serializationContext) const;

    /**
     * The size of a query shape is important, since we store these in space-constrained
     * environments like the query stats store.
     */
    size_t size() const;

    /**
     * This should be overriden by a child class if it has members whose sizes are not included in
     * specificComponents().size().
     */
    virtual size_t extraSize() const {
        return 0;
    }
    template <typename H>
    friend H AbslHashValue(H h, const Shape& shape) {
        h = H::combine(std::move(h), shape.nssOrUUID, shape.specificComponents());
        if (!shape.collation.isEmpty())
            h = H::combine(std::move(h), simpleHash(shape.collation));
        return h;
    }


    // Not shapified but it is an identifier so it may be transformed.
    NamespaceStringOrUUID nssOrUUID;

    // Never shapified. If it's empty, leave it off.
    BSONObj collation;

protected:
    Shape(NamespaceStringOrUUID, BSONObj collation_);

    /**
     * Along with the hash implementation, this is the main way that shapes are 'shapified' -
     * sub-classes should implement this to add the shapified versions of their literals to an
     * object. Depending on 'opts', this may be eligible to be used for output in $queryStats or as
     * the object to compute the QueryShapeHash.
     */
    virtual void appendCmdSpecificShapeComponents(BSONObjBuilder&,
                                                  OperationContext*,
                                                  const SerializationOptions& opts) const = 0;

private:
    void appendCmdNsOrUUID(BSONObjBuilder&,
                           const SerializationOptions&,
                           const SerializationContext& serializationContext) const;
    void appendCmdNs(BSONObjBuilder&, const NamespaceString&, const SerializationOptions&) const;
};

}  // namespace mongo::query_shape
