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

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/query_stats/key.h"
#include "mongo/db/query/write_ops/write_ops_gen.h"
#include "mongo/db/shard_role/shard_catalog/collection_type.h"
#include "mongo/util/modules.h"

namespace mongo::query_stats {

/**
 * Struct representing the insert command's unique arguments that are included in the query stats
 * key. Tracks 'ordered' and 'bypassDocumentValidation'. Other fields (maxTimeMS, writeConcern,
 * comment) are handled by the base Key class.
 */
struct InsertCmdComponents : public SpecificKeyComponents {
    explicit InsertCmdComponents(const write_ops::InsertCommandRequest& request);

    void HashValue(absl::HashState state) const final;

    void appendTo(BSONObjBuilder& bob, const SerializationOptions& opts) const;

    size_t size() const final;

    bool _ordered;
    bool _bypassDocumentValidation;
};

/**
 * An implementation of the query stats store key for the insert command. Wraps the base 'Key'
 * class and 'InsertCmdShape', and includes InsertCmdComponents for insert-specific fields
 * (ordered, bypassDocumentValidation).
 */
class InsertKey final : public Key {
public:
    InsertKey(OperationContext* opCtx,
              const write_ops::InsertCommandRequest& request,
              std::unique_ptr<query_shape::Shape> insertShape,
              query_shape::CollectionType collectionType = query_shape::CollectionType::kUnknown);

    const SpecificKeyComponents& specificComponents() const final {
        return _components;
    }

    template <typename H>
    friend H AbslHashValue(H h, const std::unique_ptr<const InsertKey>& key) {
        return H::combine(std::move(h), *key);
    }
    template <typename H>
    friend H AbslHashValue(H h, const std::shared_ptr<const InsertKey>& key) {
        return H::combine(std::move(h), *key);
    }

protected:
    void appendCommandSpecificComponents(BSONObjBuilder& bob,
                                         const SerializationOptions& opts) const final;

private:
    const InsertCmdComponents _components;
};

static_assert(sizeof(InsertKey) == sizeof(Key) + sizeof(InsertCmdComponents),
              "If the class' members have changed, this assert may need to be updated.");

}  // namespace mongo::query_stats
