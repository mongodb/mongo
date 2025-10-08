/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/extension/public/api.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/util/modules.h"

namespace mongo::extension::host {
/**
 * QueryShapeOptsAdapter is an adapter of ::MongoHostQueryShapeOpts,
 * providing host serialization options to extensions.
 */
class QueryShapeOptsAdapter final : public ::MongoHostQueryShapeOpts {
public:
    QueryShapeOptsAdapter(const SerializationOptions* opts)
        : ::MongoHostQueryShapeOpts{&VTABLE}, _opts(opts) {}

    const SerializationOptions* getOptsImpl() const {
        return _opts;
    }

private:
    static MongoExtensionStatus* _extSerializeIdentifier(const ::MongoHostQueryShapeOpts* ctx,
                                                         const ::MongoExtensionByteView* identifier,
                                                         ::MongoExtensionByteBuf** output) noexcept;

    static MongoExtensionStatus* _extSerializeFieldPath(const ::MongoHostQueryShapeOpts* ctx,
                                                        const ::MongoExtensionByteView* fieldPath,
                                                        ::MongoExtensionByteBuf** output) noexcept;

    static MongoExtensionStatus* _extSerializeLiteral(
        const ::MongoHostQueryShapeOpts* ctx,
        const ::MongoExtensionByteView* bsonElementPtr,
        ::MongoExtensionByteBuf** output) noexcept;

    static constexpr ::MongoHostQueryShapeOptsVTable VTABLE{
        &_extSerializeIdentifier, &_extSerializeFieldPath, &_extSerializeLiteral};

    const SerializationOptions* _opts;
};
}  // namespace mongo::extension::host
