// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/query/query_shape/find_cmd_shape.h"
#include "mongo/db/query/query_stats/key.h"
#include "mongo/util/modules.h"

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

    void appendTo(BSONObjBuilder& bob, const query_shape::SerializationOptions& opts) const;

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
              collectionType,
              request.getOriginalQueryShapeHash()),
          _components(request) {}

    const SpecificKeyComponents& specificComponents() const override {
        return _components;
    }

private:
    void appendCommandSpecificComponents(
        BSONObjBuilder& bob, const query_shape::SerializationOptions& opts) const final {
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
