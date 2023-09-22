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

#include <memory>

#include "mongo/db/operation_context.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/query/query_shape/find_cmd_shape.h"
#include "mongo/db/query/query_stats/key_generator.h"

namespace mongo::query_stats {

struct FindCmdQueryStatsStoreKeyComponents : public SpecificKeyComponents {
    FindCmdQueryStatsStoreKeyComponents(const FindCommandRequest* findCmd)
        : _shapifiedReadConcern(shapifyReadConcern(findCmd->getReadConcern().value_or(BSONObj()))),
          _allowPartialResults(findCmd->getAllowPartialResults().value_or(false)),
          _hasField{
              .readConcern = findCmd->getReadConcern().has_value(),
              .batchSize = findCmd->getBatchSize().has_value(),
              .maxTimeMS = findCmd->getMaxTimeMS().has_value(),
              .allowPartialResults = findCmd->getAllowPartialResults().has_value(),
              .noCursorTimeout = findCmd->getNoCursorTimeout().has_value(),
          } {}

    /**
     * Returns a copy of the read concern object. If there is an "afterClusterTime" component, the
     * timestamp is shapified according to 'opts'.
     */
    static BSONObj shapifyReadConcern(
        const BSONObj& readConcern,
        const SerializationOptions& opts =
            SerializationOptions::kRepresentativeQueryShapeSerializeOptions);

    std::int64_t size() const {
        return _hasField.readConcern ? _shapifiedReadConcern.objsize() : 0;
    }

    void HashValue(absl::HashState state) const final {
        absl::HashState::combine(
            std::move(state), _hasField, simpleHash(_shapifiedReadConcern), _allowPartialResults);
    }

    void appendTo(BSONObjBuilder& bob, const SerializationOptions& opts) const;

    // Avoid using boost::optional here because it creates extra padding at the beginning of the
    // struct. Since each QueryStatsEntry can have its own FindKeyGenerator, it's better to
    // minimize the struct's size as much as possible.

    // Preserved literal except afterClusterTime is shapified.
    BSONObj _shapifiedReadConcern;

    // Preserved literal.
    bool _allowPartialResults;

    // This anonymous struct represents the presence of the member variables as C++ bit fields.
    // In doing so, each of these boolean values takes up 1 bit instead of 1 byte.
    struct HasField {
        bool readConcern : 1 = false;
        bool batchSize : 1 = false;
        bool maxTimeMS : 1 = false;
        bool allowPartialResults : 1 = false;
        bool noCursorTimeout : 1 = false;

        bool operator==(const HasField& other) const = default;

    } _hasField;

    template <typename H>
    friend H AbslHashValue(H h, const HasField& hasField) {
        return H::combine(std::move(h),
                          hasField.readConcern,
                          hasField.batchSize,
                          hasField.maxTimeMS,
                          hasField.noCursorTimeout,
                          hasField.allowPartialResults);
    }
};

// This static assert checks to ensure that the struct's size is changed thoughtfully. If adding
// or otherwise changing the members, this assert may be updated with care.
static_assert(
    // expecting a BSONObj and one word for the allowPartialResults and another word for the
    // _hasField.
    sizeof(FindCmdQueryStatsStoreKeyComponents) <= sizeof(BSONObj) + 8 + 8,
    "Size of FindCmdQueryStatsStoreKeyComponents is too large! "
    "Make sure that the struct has been align- and padding-optimized. "
    "If the struct's members have changed, this assert may need to be updated with a new "
    "value.");

class FindKeyGenerator final : public KeyGenerator {
public:
    FindKeyGenerator(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const ParsedFindCommand& request,
        query_shape::CollectionType collectionType = query_shape::CollectionType::kUnknown)
        : KeyGenerator(expCtx->opCtx,
                       std::make_unique<query_shape::FindCmdShape>(request, expCtx),
                       request.findCommandRequest->getHint(),
                       collectionType),
          _components(request.findCommandRequest.get()) {}

    // The default implementation of hashing for smart pointers is not a good one for our purposes.
    // Here we overload them to actually take the hash of the object, rather than hashing the
    // pointer itself.
    template <typename H>
    friend H AbslHashValue(H h, const std::unique_ptr<const FindKeyGenerator>& keyGenerator) {
        return H::combine(std::move(h), *keyGenerator);
    }
    template <typename H>
    friend H AbslHashValue(H h, const std::shared_ptr<const FindKeyGenerator>& keyGenerator) {
        return H::combine(std::move(h), *keyGenerator);
    }

protected:
    const SpecificKeyComponents& specificComponents() const {
        return _components;
    }

private:
    void appendCommandSpecificComponents(BSONObjBuilder& bob,
                                         const SerializationOptions& opts) const final {
        _components.appendTo(bob, opts);
    }

    std::unique_ptr<FindCommandRequest> reparse(OperationContext* opCtx) const;

    FindCmdQueryStatsStoreKeyComponents _components;
};

// This static assert checks to ensure that the struct's size is changed thoughtfully. If adding
// or otherwise changing the members, this assert may be updated with care.
static_assert(
    sizeof(FindKeyGenerator) <= sizeof(KeyGenerator) + sizeof(BSONObj) + 2 * sizeof(int64_t),
    "Size of FindKeyGenerator is too large! "
    "Make sure that the struct has been align- and padding-optimized. "
    "If the struct's members have changed, this assert may need to be updated with a new value.");

}  // namespace mongo::query_stats
