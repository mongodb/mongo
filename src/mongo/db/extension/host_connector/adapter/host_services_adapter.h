// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/db/extension/host_connector/adapter/logger_adapter.h"
#include "mongo/db/extension/public/api.h"
#include "mongo/db/extension/shared/extension_status.h"
#include "mongo/util/concurrency/idle_thread_block.h"
#include "mongo/util/modules.h"

namespace mongo::extension::host_connector {
/**
 * IdleThreadBlockAdapter is an implementation of ::MongoExtensionIdleThreadBlock, providing the
 * ability to mark extension-spawned threads as idle, omitting them from gdb multi-threaded
 * stacktraces.
 *
 * The logic for marking the thread is done by the 'IdleThreadBlock' RAII object, and and the
 * ownership of the object is transferred to the caller.
 */
class IdleThreadBlockAdapter final : public ::MongoExtensionIdleThreadBlock {
public:
    IdleThreadBlockAdapter(const char* location)
        : ::MongoExtensionIdleThreadBlock{&VTABLE}, _idleThreadBlock{location} {
        tassert(
            11417110, "Provided location to IdleThreadBlockAdapter is null", location != nullptr);
    }

    IdleThreadBlockAdapter(const IdleThreadBlockAdapter&) = delete;
    IdleThreadBlockAdapter& operator=(const IdleThreadBlockAdapter&) = delete;
    IdleThreadBlockAdapter(IdleThreadBlockAdapter&&) = delete;
    IdleThreadBlockAdapter& operator=(IdleThreadBlockAdapter&&) = delete;

private:
    IdleThreadBlock _idleThreadBlock;
    static void _extDestroy(::MongoExtensionIdleThreadBlock* idleThreadBlock) noexcept {
        delete static_cast<IdleThreadBlockAdapter*>(idleThreadBlock);
    }

    static constexpr ::MongoExtensionIdleThreadBlockVTable VTABLE = {
        .destroy = &_extDestroy,
    };
};
/**
 * The host services implementations, shared by every negotiated capability version.
 *
 * None of the MongoExtensionHostServicesVTable entries take a 'self' pointer -- they are plain C
 * functions -- so an adapter instance cannot carry per-extension state that its own vtable
 * functions could read. The negotiated capability instead reaches them through the identity of
 * the function, by instantiating HostServicesAdapter once per version (below).
 */
namespace detail {
::MongoExtensionStatus* userAsserted(::MongoExtensionByteView structuredErrorMessage,
                                     const CapabilityVersion& capability);

::MongoExtensionStatus* tripwireAsserted(::MongoExtensionByteView structuredErrorMessage,
                                         const CapabilityVersion& capability);

::MongoExtensionStatus* markIdleThreadBlock(::MongoExtensionIdleThreadBlock** idleThreadBlock,
                                            const char* location,
                                            const CapabilityVersion& capability);

::MongoExtensionStatus* createHostAggStageParseNode(::MongoExtensionByteView spec,
                                                    ::MongoExtensionAggStageParseNode** node,
                                                    const CapabilityVersion& capability);

::MongoExtensionStatus* createIdLookup(::MongoExtensionByteView bsonSpec,
                                       ::MongoExtensionAggStageAstNode** node,
                                       const CapabilityVersion& capability);

::MongoExtensionStatus* createDocumentResultsAndMetadata(
    ::MongoExtensionByteView bsonSpec,
    ::MongoExtensionDocResultsDPLCallback dplCallback,
    void* dplCallbackUserData,
    void (*dplCallbackDestroy)(void*),
    ::MongoExtensionAggStageAstNode** node,
    const CapabilityVersion& capability);
}  // namespace detail

template <uint32_t kPacked>
class HostServicesAdapter;

/**
 * getHostServices returns the HostServicesAdapter instance for the given negotiated capability
 * version. This trampoline is the only way to obtain one: the per-version instances cannot be
 * vended without a negotiated capability.
 */
::MongoExtensionHostServices* getHostServices(const CapabilityVersion& capability);

/**
 * HostServicesAdapter is an implementation of ::MongoExtensionHostServices, providing host
 * services to extensions.
 *
 * For each function in the MongoExtensionHostServicesVTable, this adapter has a corresponding
 * function that translates between the C API and the core implementation of the host services.
 *
 * We use one instance per negotiable capability version. The version is baked into the type (and
 * exposed via kCapability) rather than stored in a member because none of the vtable entries
 * receive a 'self' pointer, so a member would be unreachable from the very functions that need
 * it. Each instantiation gets its own VTABLE whose entries point at that instantiation's statics,
 * which is what makes kCapability recoverable inside them.
 *
 * Instances are shared by every extension that negotiated the same version; the capability is the
 * only per-extension state these functions require. The pointer is passed to an extension during
 * initialization and is expected to be valid for the lifetime of the extension.
 *
 * kPacked must be a valid CapabilityVersion packed value representation.
 */
template <uint32_t kPacked>
class HostServicesAdapter final : public ::MongoExtensionHostServices {
public:
    static constexpr CapabilityVersion kCapability = CapabilityVersion::fromPackedValue(kPacked);

    HostServicesAdapter(const HostServicesAdapter&) = delete;
    HostServicesAdapter& operator=(const HostServicesAdapter&) = delete;
    HostServicesAdapter(HostServicesAdapter&&) = delete;
    HostServicesAdapter& operator=(HostServicesAdapter&&) = delete;

    static ::MongoExtensionHostServicesVTable getVTable() {
        return VTABLE;
    }

private:
    template <template <uint32_t> class InstanceT, typename CApiT>
    friend CApiT* mongo::extension::getNegotiatedVersionInstance(const CapabilityVersion&);

    HostServicesAdapter() : ::MongoExtensionHostServices{&VTABLE} {}

    static HostServicesAdapter& getInstance() {
        static HostServicesAdapter sInstance;
        return sInstance;
    }

    static ::MongoExtensionHostServices* getInstancePtr() {
        return &getInstance();
    }

    static ::MongoExtensionLogger* _extGetLogger() {
        return getLogger(kCapability);
    }

    static ::MongoExtensionStatus* _extUserAsserted(
        ::MongoExtensionByteView structuredErrorMessage) {
        return detail::userAsserted(structuredErrorMessage, kCapability);
    }

    static ::MongoExtensionStatus* _extTripwireAsserted(
        ::MongoExtensionByteView structuredErrorMessage) {
        return detail::tripwireAsserted(structuredErrorMessage, kCapability);
    }

    static ::MongoExtensionStatus* _extMarkIdleThreadBlock(
        ::MongoExtensionIdleThreadBlock** idleThreadBlock, const char* location) {
        return detail::markIdleThreadBlock(idleThreadBlock, location, kCapability);
    }

    static ::MongoExtensionStatus* _extCreateHostAggStageParseNode(
        ::MongoExtensionByteView spec, ::MongoExtensionAggStageParseNode** node) noexcept {
        return detail::createHostAggStageParseNode(spec, node, kCapability);
    }

    static ::MongoExtensionStatus* _extCreateIdLookup(
        ::MongoExtensionByteView bsonSpec, ::MongoExtensionAggStageAstNode** node) noexcept {
        return detail::createIdLookup(bsonSpec, node, kCapability);
    }

    static ::MongoExtensionStatus* _extCreateDocumentResultsAndMetadata(
        ::MongoExtensionByteView bsonSpec,
        ::MongoExtensionDocResultsDPLCallback dplCallback,
        void* dplCallbackUserData,
        void (*dplCallbackDestroy)(void*),
        ::MongoExtensionAggStageAstNode** node) noexcept {
        return detail::createDocumentResultsAndMetadata(
            bsonSpec, dplCallback, dplCallbackUserData, dplCallbackDestroy, node, kCapability);
    }

    static constexpr ::MongoExtensionHostServicesVTable VTABLE = {
        .get_logger = &_extGetLogger,
        .user_asserted = &_extUserAsserted,
        .tripwire_asserted = &_extTripwireAsserted,
        .mark_idle_thread_block = &_extMarkIdleThreadBlock,
        .create_host_agg_stage_parse_node = &_extCreateHostAggStageParseNode,
        .create_id_lookup = &_extCreateIdLookup,
        .create_document_results_and_metadata = &_extCreateDocumentResultsAndMetadata};
};

inline ::MongoExtensionHostServices* getHostServices(const CapabilityVersion& capability) {
    // One HostServicesAdapter instantiation per negotiable (major, minor), in a row-major table
    // with one row per supported major and one column per negotiable minor of that major. The
    // table holds factories so each instance is constructed lazily, on first negotiation of its
    // version. This mirrors getExtensionStatusOK; the row-size and offset helpers it uses are not
    // StatusOK-specific, they just walk kSupportedVersionMaxima.
    return getNegotiatedVersionInstance<HostServicesAdapter, ::MongoExtensionHostServices>(
        capability);
}
}  // namespace mongo::extension::host_connector
