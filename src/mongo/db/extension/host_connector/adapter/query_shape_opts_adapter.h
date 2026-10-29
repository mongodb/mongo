// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/db/extension/public/api.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/util/modules.h"

namespace mongo::extension::host_connector {
/**
 * QueryShapeOptsAdapter is an adapter to ::MongoExtensionHostQueryShapeOpts,
 * providing host serialization options to extensions.
 */
class QueryShapeOptsAdapter final : public ::MongoExtensionHostQueryShapeOpts {
public:
    QueryShapeOptsAdapter(const query_shape::SerializationOptions* opts,
                          boost::intrusive_ptr<ExpressionContext> expCtx)
        : ::MongoExtensionHostQueryShapeOpts{&VTABLE}, _opts(opts), _expCtx(std::move(expCtx)) {
        uassert(11717605, "ExpressionContext pointer cannot be null", _expCtx != nullptr);
    }

    const query_shape::SerializationOptions* getOptsImpl() const {
        return _opts;
    }

    const boost::intrusive_ptr<ExpressionContext>& getExpCtx() const {
        return _expCtx;
    }

    static inline bool isHostAllocated(const ::MongoExtensionHostQueryShapeOpts& opts) {
        return opts.vtable == &VTABLE;
    }

private:
    static MongoExtensionStatus* _extSerializeIdentifier(
        const ::MongoExtensionHostQueryShapeOpts* ctx,
        ::MongoExtensionByteView identifier,
        ::MongoExtensionByteBuf** output) noexcept;

    static MongoExtensionStatus* _extSerializeFieldPath(
        const ::MongoExtensionHostQueryShapeOpts* ctx,
        ::MongoExtensionByteView fieldPath,
        ::MongoExtensionByteBuf** output) noexcept;

    static MongoExtensionStatus* _extSerializeLiteral(const ::MongoExtensionHostQueryShapeOpts* ctx,
                                                      ::MongoExtensionByteView bsonElement,
                                                      ::MongoExtensionByteBuf** output) noexcept;

    static constexpr ::MongoExtensionHostQueryShapeOptsVTable VTABLE = {
        .serialize_identifier = &_extSerializeIdentifier,
        .serialize_field_path = &_extSerializeFieldPath,
        .serialize_literal = &_extSerializeLiteral,
    };

    const query_shape::SerializationOptions* _opts;
    boost::intrusive_ptr<ExpressionContext> _expCtx;
};
}  // namespace mongo::extension::host_connector
