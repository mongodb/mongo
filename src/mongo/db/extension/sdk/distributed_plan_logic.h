// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once
#include "mongo/bson/bsonobj.h"
#include "mongo/db/extension/public/api.h"
#include "mongo/db/extension/sdk/dpl_array_container.h"
#include "mongo/db/extension/shared/byte_buf.h"
#include "mongo/db/extension/shared/extension_status.h"
#include "mongo/util/modules.h"

namespace mongo::extension::sdk {

/**
 * DistributedPlanLogic implements the ::MongoExtensionDistributedPlanLogic interface for an
 * extension.
 */
class DistributedPlanLogic final {
public:
    DistributedPlanLogic() : shardsPipeline({}), mergingPipeline({}), sortPattern() {}

    DistributedPlanLogic(DPLArrayContainer&& shardsPipeline,
                         DPLArrayContainer&& mergingPipeline,
                         BSONObj sortPattern)
        : shardsPipeline(std::move(shardsPipeline)),
          mergingPipeline(std::move(mergingPipeline)),
          sortPattern(sortPattern.getOwned()) {}

    /**
     * Pipeline to execute on each shard in parallel.
     */
    DPLArrayContainer shardsPipeline;
    /**
     * Pipeline to execute on the merging node.
     */
    DPLArrayContainer mergingPipeline;
    /**
     * Sort pattern describing how shard results are sorted when they arrive at the merging node.
     * The sharding merge step uses this to merge-sort the incoming shard streams.
     *
     * This is distinct from LogicalAggStage::getSortPattern(), which describes the sort order of
     * this stage's final output (post-merge). For source stages (no input required), these two
     * values must be equal; for transform stages they can differ when the merging pipeline changes
     * the sort order.
     */
    BSONObj sortPattern;
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
    ExtensionDistributedPlanLogicAdapter(DistributedPlanLogic&& dpl)
        : ::MongoExtensionDistributedPlanLogic{&VTABLE}, _dpl(std::move(dpl)) {}

    ~ExtensionDistributedPlanLogicAdapter() = default;

    ExtensionDistributedPlanLogicAdapter(const ExtensionDistributedPlanLogicAdapter&) = delete;
    ExtensionDistributedPlanLogicAdapter& operator=(const ExtensionDistributedPlanLogicAdapter&) =
        delete;
    ExtensionDistributedPlanLogicAdapter(ExtensionDistributedPlanLogicAdapter&&) = delete;
    ExtensionDistributedPlanLogicAdapter& operator=(ExtensionDistributedPlanLogicAdapter&&) =
        delete;

    static ::MongoExtensionDistributedPlanLogicVTable getVTable() {
        return VTABLE;
    }

private:
    const DistributedPlanLogic& getImpl() const noexcept {
        return _dpl;
    }

    DistributedPlanLogic& getImpl() noexcept {
        return _dpl;
    }

    static void _extDestroy(::MongoExtensionDistributedPlanLogic* distributedPlanLogic) noexcept {
        delete static_cast<ExtensionDistributedPlanLogicAdapter*>(distributedPlanLogic);
    }

    static ::MongoExtensionStatus* _extExtractShardsPipeline(
        ::MongoExtensionDistributedPlanLogic* distributedPlanLogic,
        ::MongoExtensionDPLArrayContainer** output) noexcept {
        return wrapCXXAndConvertExceptionToStatus([&]() {
            *output = nullptr;
            auto& impl =
                static_cast<ExtensionDistributedPlanLogicAdapter*>(distributedPlanLogic)->getImpl();

            if (impl.shardsPipeline.size() > 0) {
                // Transfer ownership of the container to the caller.
                *output = new ExtensionDPLArrayContainerAdapter(std::move(impl.shardsPipeline));
            }
        });
    }

    static ::MongoExtensionStatus* _extExtractMergingPipeline(
        ::MongoExtensionDistributedPlanLogic* distributedPlanLogic,
        ::MongoExtensionDPLArrayContainer** output) noexcept {
        return wrapCXXAndConvertExceptionToStatus([&]() {
            *output = nullptr;
            auto& impl =
                static_cast<ExtensionDistributedPlanLogicAdapter*>(distributedPlanLogic)->getImpl();

            if (impl.mergingPipeline.size() > 0) {
                // Transfer ownership of the container to the caller.
                *output = new ExtensionDPLArrayContainerAdapter(std::move(impl.mergingPipeline));
            }
        });
    }

    static ::MongoExtensionStatus* _extGetSortPattern(
        const ::MongoExtensionDistributedPlanLogic* distributedPlanLogic,
        ::MongoExtensionByteBuf** output) noexcept {
        return wrapCXXAndConvertExceptionToStatus([&]() {
            *output = nullptr;
            const auto& impl =
                static_cast<const ExtensionDistributedPlanLogicAdapter*>(distributedPlanLogic)
                    ->getImpl();

            if (!impl.sortPattern.isEmpty()) {
                *output = new ByteBuf(impl.sortPattern);
            }
        });
    }

    static constexpr ::MongoExtensionDistributedPlanLogicVTable VTABLE = {
        .destroy = &_extDestroy,
        .extract_shards_pipeline = &_extExtractShardsPipeline,
        .extract_merging_pipeline = &_extExtractMergingPipeline,
        .get_sort_pattern = &_extGetSortPattern};

    DistributedPlanLogic _dpl;
};

}  // namespace mongo::extension::sdk
