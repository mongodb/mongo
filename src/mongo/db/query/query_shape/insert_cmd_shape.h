/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/query/query_shape/query_shape.h"
#include "mongo/db/query/write_ops/write_ops_gen.h"
#include "mongo/util/modules.h"

namespace mongo::query_shape {

/**
 * Struct representing the insert command's specific shape components. The 'documents' field is
 * always shapified as a placeholder array of objects (?array<?object>). No other insert-specific
 * fields are part of the shape.
 */
struct InsertCmdShapeComponents : public CmdSpecificShapeComponents {
    InsertCmdShapeComponents() = default;

    void HashValue(absl::HashState state) const final {
        // documents is always the same fixed placeholder - nothing to hash.
    }

    size_t size() const final {
        return sizeof(InsertCmdShapeComponents);
    }

    void appendTo(BSONObjBuilder& bob, const SerializationOptions& opts) const;
};

/**
 * A class representing the query shape of an insert command. The 'insert' (collection name) field
 * is included as-is via the base class namespace handling. The 'documents' field is always
 * shapified as a placeholder (?array<?object>). All other fields are excluded.
 */
class InsertCmdShape final : public Shape {
public:
    explicit InsertCmdShape(const write_ops::InsertCommandRequest& request);

    const CmdSpecificShapeComponents& specificComponents() const final {
        return _components;
    }

protected:
    void appendCmdSpecificShapeComponents(BSONObjBuilder& bob,
                                          OperationContext* opCtx,
                                          const SerializationOptions& opts) const final;

private:
    InsertCmdShapeComponents _components;
};

static_assert(sizeof(InsertCmdShape) == sizeof(Shape) + sizeof(InsertCmdShapeComponents),
              "If the class' members have changed, this assert may need to be updated.");

}  // namespace mongo::query_shape
