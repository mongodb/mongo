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

#include "mongo/bson/bsonobj.h"
#include "mongo/db/extension/public/api.h"
#include "mongo/db/extension/shared/handle/aggregation_stage/executable_agg_stage.h"
#include "mongo/db/extension/shared/handle/handle.h"
#include "mongo/db/query/explain_options.h"
#include "mongo/util/modules.h"

namespace mongo::extension {

/**
 * LogicalAggStageHandle is an owned handle wrapper around a
 * MongoExtensionLogicalAggStage.
 */
class LogicalAggStageHandle : public OwnedHandle<::MongoExtensionLogicalAggStage> {
public:
    LogicalAggStageHandle(::MongoExtensionLogicalAggStage* ptr)
        : OwnedHandle<::MongoExtensionLogicalAggStage>(ptr) {
        _assertValidVTable();
    }

    BSONObj serialize() const;

    /**
     * Collects explain output at the specified verbosity from this logical stage.
     */
    BSONObj explain(ExplainOptions::Verbosity verbosity) const;

    /**
     * Compiles a logical stage into an execution stage.
     */
    ExecAggStageHandle compile() const;

protected:
    void _assertVTableConstraints(const VTable_t& vtable) const override {
        tassert(
            11173703, "ExtensionLogicalAggStage 'serialize' is null", vtable.serialize != nullptr);
        tassert(11239401, "ExtensionLogicalAggStage 'explain' is null", vtable.explain != nullptr);
        tassert(10957200, "ExtensionLogicalAggStage 'compile' is null", vtable.compile != nullptr);
    }
};
}  // namespace mongo::extension
