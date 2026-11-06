// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/query_shape/find_and_modify_cmd_shape.h"
#include "mongo/db/query/query_stats/key.h"
#include "mongo/db/query/write_ops/write_ops_gen.h"
#include "mongo/util/modules.h"

#include <memory>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo::query_stats {

/**
 * Struct representing the unique arguments that are included in the query stats key for
 * findAndModify. Tracks 'new' and 'bypassDocumentValidation'; unlike the batch write commands,
 * findAndModify has no 'ordered' field. Other fields (maxTimeMS, writeConcern, comment, hint) are
 * handled by the base Key class.
 */
struct FindAndModifyCmdComponents : public SpecificKeyComponents {
    explicit FindAndModifyCmdComponents(const write_ops::FindAndModifyCommandRequest& request);

    void HashValue(absl::HashState state) const final;

    void appendTo(BSONObjBuilder& bob, const query_shape::SerializationOptions& opts) const;

    size_t size() const final;

    // Whether the command requested the modified rather than the original document. Kept
    // unmodified and deliberately excluded from the query shape.
    bool _new;

    bool _bypassDocumentValidation;
};

/**
 * An implementation of the query stats store key for findAndModify. Wraps the base 'Key' class
 * and includes FindAndModifyCmdComponents for the command-specific fields ('new' and
 * 'bypassDocumentValidation'). A dedicated key is used rather than WriteKey<Request> because the
 * latter's template assumes an 'ordered' field, which findAndModify does not have.
 */
class FindAndModifyKey final : public Key {
public:
    FindAndModifyKey(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const write_ops::FindAndModifyCommandRequest& request,
        std::unique_ptr<query_shape::Shape> shape,
        query_shape::CollectionType collectionType = query_shape::CollectionType::kUnknown);

    const SpecificKeyComponents& specificComponents() const final {
        return _components;
    }

protected:
    void appendCommandSpecificComponents(BSONObjBuilder& bob,
                                         const query_shape::SerializationOptions& opts) const final;

private:
    const FindAndModifyCmdComponents _components;
};

static_assert(sizeof(FindAndModifyKey) == sizeof(Key) + sizeof(FindAndModifyCmdComponents),
              "If the class' members have changed, this assert may need to be updated.");

}  // namespace mongo::query_stats
