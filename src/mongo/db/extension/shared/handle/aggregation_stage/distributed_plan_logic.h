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
#include "mongo/db/extension/shared/handle/aggregation_stage/parse_node.h"
#include "mongo/db/extension/shared/handle/byte_buf_handle.h"
#include "mongo/db/extension/shared/handle/handle.h"
#include "mongo/util/modules.h"

namespace mongo::extension {

/**
 * DistributedPlanLogicHandle is an owned handle wrapper around a
 * MongoExtensionDistributedPlanLogic.
 */
class DistributedPlanLogicHandle : public OwnedHandle<::MongoExtensionDistributedPlanLogic> {
public:
    DistributedPlanLogicHandle(::MongoExtensionDistributedPlanLogic* dpl)
        : OwnedHandle<::MongoExtensionDistributedPlanLogic>(dpl) {
        _assertValidVTable();
    }

    std::vector<VariantDPLHandle> getShardsPipeline() const;

    std::vector<VariantDPLHandle> getMergingPipeline() const;

    BSONObj getSortPattern() const;

protected:
    void _assertVTableConstraints(const VTable_t& vtable) const override {
        tassert(11027300,
                "DistributedPlanLogic 'get_shards_pipeline' is null",
                vtable.get_shards_pipeline != nullptr);
        tassert(11027301,
                "DistributedPlanLogic 'get_merging_pipeline' is null",
                vtable.get_merging_pipeline != nullptr);
        tassert(11027302,
                "DistributedPlanLogic 'get_sort_pattern' is null",
                vtable.get_sort_pattern != nullptr);
    }
};

}  // namespace mongo::extension

