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

#include "mongo/db/operation_context.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/query/query_shape/find_cmd_shape.h"
#include "mongo/db/query/query_stats/key.h"

#include <memory>

namespace mongo::query_stats {

struct FindCmdComponents : public SpecificKeyComponents {
    FindCmdComponents(const FindCommandRequest& request)
        : _allowPartialResults(request.getAllowPartialResults().value_or(false)),
          _noCursorTimeout(request.getNoCursorTimeout().value_or(false)),
          _hasField{
              .batchSize = request.getBatchSize().has_value(),
              .allowPartialResults = request.getAllowPartialResults().has_value(),
              .noCursorTimeout = request.getNoCursorTimeout().has_value(),
          } {}


    std::size_t size() const override {
        return sizeof(FindCmdComponents);
    }

    void HashValue(absl::HashState state) const final {
        absl::HashState::combine(
            std::move(state), _hasField, _allowPartialResults, _noCursorTimeout);
    }

    void appendTo(BSONObjBuilder& bob, const SerializationOptions& opts) const;

    // Avoid using boost::optional here because it creates extra padding at the beginning of the
    // struct. Since each QueryStatsEntry can have its own FindKey, it's better to
    // minimize the struct's size as much as possible.

    // Preserved literal.
    bool _allowPartialResults;
    bool _noCursorTimeout;

    // This anonymous struct represents the presence of the member variables as C++ bit fields.
    // In doing so, each of these boolean values takes up 1 bit instead of 1 byte.
    struct HasField {
        bool batchSize : 1 = false;
        bool allowPartialResults : 1 = false;
        bool noCursorTimeout : 1 = false;
        bool operator==(const HasField& other) const = default;

    } _hasField;

    template <typename H>
    friend H AbslHashValue(H h, const HasField& hasField) {
        return H::combine(std::move(h),
                          hasField.batchSize,
                          hasField.noCursorTimeout,
                          hasField.allowPartialResults);
    }
};

// This static assert checks to ensure that the struct's size is changed thoughtfully. If adding
// or otherwise changing the members, this assert may be updated with care.
static_assert(
    // Expecting two bytes for allowPartialResults and noCursorTimeout, and another
    // byte for _hasField. For alignment reasons (alignment is 8 bytes here), this means the trailer
    // will bring up the total bytecount to a multiple of 8.
    sizeof(FindCmdComponents) <= sizeof(SpecificKeyComponents) + 8,
    "Size of FindCmdComponents is too large! "
    "Make sure that the struct has been align- and padding-optimized. "
    "If the struct's members have changed, this assert may need to be updated with a new "
    "value.");

class FindKey final : public Key {
public:
    FindKey(const boost::intrusive_ptr<ExpressionContext>& expCtx,
            const FindCommandRequest& request,
            std::unique_ptr<query_shape::Shape> findShape,
            query_shape::CollectionType collectionType = query_shape::CollectionType::kUnknown)
        : Key(expCtx->getOperationContext(),
              std::move(findShape),
              request.getHint(),
              request.getReadConcern(),
              request.getMaxTimeMS().has_value(),
              collectionType),
          _components(request) {}

    // The default implementation of hashing for smart pointers is not a good one for our purposes.
    // Here we overload them to actually take the hash of the object, rather than hashing the
    // pointer itself.
    template <typename H>
    friend H AbslHashValue(H h, const std::unique_ptr<const FindKey>& key) {
        return H::combine(std::move(h), *key);
    }
    template <typename H>
    friend H AbslHashValue(H h, const std::shared_ptr<const FindKey>& key) {
        return H::combine(std::move(h), *key);
    }

    const SpecificKeyComponents& specificComponents() const override {
        return _components;
    }

private:
    void appendCommandSpecificComponents(BSONObjBuilder& bob,
                                         const SerializationOptions& opts) const final {
        _components.appendTo(bob, opts);
    }

    FindCmdComponents _components;
};

// This static assert checks to ensure that the struct's size is changed thoughtfully. If adding
// or otherwise changing the members, this assert may be updated with care.
static_assert(sizeof(FindKey) == sizeof(Key) + sizeof(FindCmdComponents),
              "If the class' members have changed, this assert may need to be updated with a new "
              "value and the size calcuation will need to be changed.");

}  // namespace mongo::query_stats
