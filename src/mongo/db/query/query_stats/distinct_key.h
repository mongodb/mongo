/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/query/query_shape/distinct_cmd_shape.h"
#include "mongo/db/query/query_stats/key.h"

#include <memory>


namespace mongo::query_stats {

/**
 * An implementation of the query stats store key for the distinct command. This class is a simple
 * wrapper around the base 'Key' class and 'DistinctCmdShape', since the distinct command doesn't
 * include any command- specific components in its query stats store key beyond the universal
 * components (e.g. hint, read concern).
 */
class DistinctKey final : public Key {
public:
    DistinctKey(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                const DistinctCommandRequest& request,
                std::unique_ptr<query_shape::Shape> distinctShape,
                query_shape::CollectionType collectionType = query_shape::CollectionType::kUnknown)
        : Key(expCtx->getOperationContext(),
              std::move(distinctShape),
              request.getHint(),
              request.getReadConcern(),
              request.getMaxTimeMS().has_value(),
              collectionType),
          _components() {}

    // The default implementation of hashing for smart pointers is not a good one for our purposes.
    // Here we overload them to actually take the hash of the object, rather than hashing the
    // pointer itself.
    template <typename H>
    friend H AbslHashValue(H h, const std::unique_ptr<const DistinctKey>& key) {
        return H::combine(std::move(h), *key);
    }
    template <typename H>
    friend H AbslHashValue(H h, const std::shared_ptr<const DistinctKey>& key) {
        return H::combine(std::move(h), *key);
    }

    const SpecificKeyComponents& specificComponents() const override {
        return _components;
    }

private:
    void appendCommandSpecificComponents(BSONObjBuilder& bob,
                                         const SerializationOptions& opts) const final {}

    EmptyCmdComponents _components;
};

// This static assert checks to ensure that the struct's size is changed thoughtfully. If adding
// or otherwise changing the members, this assert may be updated with care.
static_assert(sizeof(DistinctKey) == sizeof(Key) + sizeof(EmptyCmdComponents),
              "If the class' members have changed, this assert may need to be updated with a new "
              "value and the size calcuation will need to be changed.");

}  // namespace mongo::query_stats
