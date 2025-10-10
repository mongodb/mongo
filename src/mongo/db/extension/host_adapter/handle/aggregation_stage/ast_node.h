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

#include "mongo/db/extension/host_adapter/handle/aggregation_stage/logical.h"
#include "mongo/db/extension/public/api.h"
#include "mongo/db/extension/shared/byte_buf_utils.h"
#include "mongo/db/extension/shared/handle/handle.h"
#include "mongo/util/modules.h"

#include <absl/base/nullability.h>

namespace mongo::extension::host_adapter {

/**
 * AggStageAstNodeHandle is an owned handle wrapper around a
 * MongoExtensionAggStageAstNode.
 */
class AggStageAstNodeHandle : public OwnedHandle<::MongoExtensionAggStageAstNode> {
public:
    AggStageAstNodeHandle(::MongoExtensionAggStageAstNode* ptr)
        : OwnedHandle<::MongoExtensionAggStageAstNode>(ptr) {
        _assertValidVTable();
    }

    /**
     * Returns a StringData containing the name of this aggregation stage.
     */
    StringData getName() const {
        auto stringView = byteViewAsStringView(vtable().get_name(get()));
        return StringData{stringView.data(), stringView.size()};
    }

    /**
     * Returns a logical stage with the stage's runtime implementation of the optimization
     * interface.
     *
     * On success, the logical stage is returned and belongs to the caller.
     * On failure, the error triggers an assertion.
     *
     */
    LogicalAggStageHandle bind() const;

protected:
    void _assertVTableConstraints(const VTable_t& vtable) const override {
        tassert(
            11217601, "ExtensionAggStageAstNode 'get_name' is null", vtable.get_name != nullptr);
        tassert(11113700, "ExtensionAggStageAstNode 'bind' is null", vtable.bind != nullptr);
    }
};
}  // namespace mongo::extension::host_adapter
