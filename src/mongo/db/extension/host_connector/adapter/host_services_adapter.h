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
 * HostServicesAdapter is an implementation of ::MongoExtensionHostServices, providing host
 * services to extensions.
 *
 * For each function in the MongoExtensionHostServicesVTable, this adapter has a corresponding
 * function that translates between the C API and the core implementation of the host services.
 *
 * The HostServicesAdapter instance is a singleton, and is accessible via
 * HostServicesAdapter::get(). The pointer to the singleton instance is passed to extensions
 * during initialization, and is expected to be valid for the lifetime of the extension.
 */
class HostServicesAdapter final : public ::MongoExtensionHostServices {
public:
    HostServicesAdapter() : ::MongoExtensionHostServices{&VTABLE} {}

    HostServicesAdapter(const HostServicesAdapter&) = delete;
    HostServicesAdapter& operator=(const HostServicesAdapter&) = delete;
    HostServicesAdapter(HostServicesAdapter&&) = delete;
    HostServicesAdapter& operator=(HostServicesAdapter&&) = delete;

    static HostServicesAdapter& get() {
        static HostServicesAdapter sInstance;
        return sInstance;
    }

private:
    static ::MongoExtensionLogger* _extGetLogger() {
        return LoggerAdapter::get();
    }

    static ::MongoExtensionStatus* _extUserAsserted(
        ::MongoExtensionByteView structuredErrorMessage);

    static ::MongoExtensionStatus* _extMarkIdleThreadBlock(
        ::MongoExtensionIdleThreadBlock** idleThreadBlock, const char* location) {
        return wrapCXXAndConvertExceptionToStatus(
            [&]() { *idleThreadBlock = new IdleThreadBlockAdapter(location); });
    }

    static ::MongoExtensionStatus* _extTripwireAsserted(
        ::MongoExtensionByteView structuredErrorMessage);

    static MongoExtensionStatus* _extCreateHostAggStageParseNode(
        ::MongoExtensionByteView spec, ::MongoExtensionAggStageParseNode** node) noexcept;

    static ::MongoExtensionStatus* _extCreateIdLookup(
        ::MongoExtensionByteView bsonSpec, ::MongoExtensionAggStageAstNode** node) noexcept;

    static ::MongoExtensionStatus* _extCreateDocumentResultsAndMetadata(
        ::MongoExtensionByteView bsonSpec,
        ::MongoExtensionDocResultsDPLCallback dplCallback,
        void* dplCallbackUserData,
        void (*dplCallbackDestroy)(void*),
        ::MongoExtensionAggStageAstNode** node) noexcept;

    static constexpr ::MongoExtensionHostServicesVTable VTABLE = {
        .get_logger = &_extGetLogger,
        .user_asserted = &_extUserAsserted,
        .tripwire_asserted = &_extTripwireAsserted,
        .mark_idle_thread_block = &_extMarkIdleThreadBlock,
        .create_host_agg_stage_parse_node = &_extCreateHostAggStageParseNode,
        .create_id_lookup = &_extCreateIdLookup,
        .create_document_results_and_metadata = &_extCreateDocumentResultsAndMetadata};
};
}  // namespace mongo::extension::host_connector
