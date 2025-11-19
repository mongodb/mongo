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
#include "mongo/db/extension/sdk/dpl_array_container.h"
#include "mongo/db/extension/shared/byte_buf.h"
#include "mongo/db/extension/shared/extension_status.h"
#include "mongo/util/modules.h"

namespace mongo::extension::sdk {

/**
 * DistributedPlanLogicBase is the base class for implementing the
 * ::MongoExtensionDistributedPlanLogic interface by an extension.
 */
class DistributedPlanLogicBase {
public:
    virtual ~DistributedPlanLogicBase() = default;

    /**
     * Returns the pipeline to execute on each shard in parallel.
     * Ownership of the returned DPLArrayContainer is transferred to the caller.
     */
    virtual std::unique_ptr<DPLArrayContainer> getShardsPipeline() const = 0;

    /**
     * Returns the pipeline to execute on the merging node.
     * Ownership of the returned DPLArrayContainer is transferred to the caller.
     */
    virtual std::unique_ptr<DPLArrayContainer> getMergingPipeline() const = 0;

    /**
     * Returns which fields are ascending and which fields are descending when merging streams
     * together.
     */
    virtual BSONObj getSortPattern() const = 0;
};

/**
 * ExtensionDistributedPlanLogicAdapter is a boundary object representation of a
 * ::MongoExtensionDistributedPlanLogic. It is meant to abstract away the C++ implementation
 * by the extension and provides the interface at the API boundary which will be called upon by the
 * host. The static VTABLE member points to static methods which ensure the correct conversion from
 * C++ context to the C API context.
 */
class ExtensionDistributedPlanLogicAdapter final : public ::MongoExtensionDistributedPlanLogic {
public:
    ExtensionDistributedPlanLogicAdapter(std::unique_ptr<DistributedPlanLogicBase> dpl)
        : ::MongoExtensionDistributedPlanLogic{&VTABLE}, _dpl(std::move(dpl)) {
        tassert(11027303, "Provided DistributedPlanLogicBase is null", _dpl != nullptr);
    }

    ~ExtensionDistributedPlanLogicAdapter() = default;

    // Delete copy and move constructors/assignment to guarantee _dpl remains valid
    // throughout the object's lifetime. Adapters should only be accessed through Handles, so
    // copying/moving is unnecessary.
    ExtensionDistributedPlanLogicAdapter(const ExtensionDistributedPlanLogicAdapter&) = delete;
    ExtensionDistributedPlanLogicAdapter& operator=(const ExtensionDistributedPlanLogicAdapter&) =
        delete;
    ExtensionDistributedPlanLogicAdapter(ExtensionDistributedPlanLogicAdapter&&) = delete;
    ExtensionDistributedPlanLogicAdapter& operator=(ExtensionDistributedPlanLogicAdapter&&) =
        delete;

private:
    const DistributedPlanLogicBase& getImpl() const noexcept {
        return *_dpl;
    }

    DistributedPlanLogicBase& getImpl() noexcept {
        return *_dpl;
    }

    static void _extDestroy(::MongoExtensionDistributedPlanLogic* distributedPlanLogic) noexcept {
        delete static_cast<ExtensionDistributedPlanLogicAdapter*>(distributedPlanLogic);
    }

    static ::MongoExtensionStatus* _extGetShardsPipeline(
        ::MongoExtensionDistributedPlanLogic* distributedPlanLogic,
        ::MongoExtensionDPLArrayContainer** output) noexcept {
        return wrapCXXAndConvertExceptionToStatus([&]() {
            *output = nullptr;
            auto& impl =
                static_cast<ExtensionDistributedPlanLogicAdapter*>(distributedPlanLogic)->getImpl();

            auto shardsPipeline = impl.getShardsPipeline();
            if (shardsPipeline) {
                // Transfer ownership of the container to the caller.
                *output = new ExtensionDPLArrayContainerAdapter(std::move(shardsPipeline));
            }
        });
    }

    static ::MongoExtensionStatus* _extGetMergingPipeline(
        ::MongoExtensionDistributedPlanLogic* distributedPlanLogic,
        ::MongoExtensionDPLArrayContainer** output) noexcept {
        return wrapCXXAndConvertExceptionToStatus([&]() {
            *output = nullptr;
            auto& impl =
                static_cast<ExtensionDistributedPlanLogicAdapter*>(distributedPlanLogic)->getImpl();

            auto mergingPipeline = impl.getMergingPipeline();
            if (mergingPipeline) {
                // Transfer ownership of the container to the caller.
                *output = new ExtensionDPLArrayContainerAdapter(std::move(mergingPipeline));
            }
        });
    }

    static ::MongoExtensionStatus* _extGetSortPattern(
        ::MongoExtensionDistributedPlanLogic* distributedPlanLogic,
        ::MongoExtensionByteBuf** output) noexcept {
        return wrapCXXAndConvertExceptionToStatus([&]() {
            *output = nullptr;
            const auto& impl =
                static_cast<const ExtensionDistributedPlanLogicAdapter*>(distributedPlanLogic)
                    ->getImpl();

            auto sortPattern = impl.getSortPattern();
            if (!sortPattern.isEmpty()) {
                *output = new VecByteBuf(sortPattern);
            }
        });
    }

    static constexpr ::MongoExtensionDistributedPlanLogicVTable VTABLE{
        .destroy = &_extDestroy,
        .get_shards_pipeline = &_extGetShardsPipeline,
        .get_merging_pipeline = &_extGetMergingPipeline,
        .get_sort_pattern = &_extGetSortPattern};

    std::unique_ptr<DistributedPlanLogicBase> _dpl;
};

}  // namespace mongo::extension::sdk
