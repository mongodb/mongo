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

    const ::MongoExtensionIdleThreadBlockVTable VTABLE{.destroy = &_extDestroy};
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

    static HostServicesAdapter* get() {
        return &_hostServicesAdapter;
    }

private:
    static HostServicesAdapter _hostServicesAdapter;

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

    static constexpr ::MongoExtensionHostServicesVTable VTABLE{
        .get_logger = &_extGetLogger,
        .user_asserted = &_extUserAsserted,
        .tripwire_asserted = &_extTripwireAsserted,
        .mark_idle_thread_block = &_extMarkIdleThreadBlock,
        .create_host_agg_stage_parse_node = &_extCreateHostAggStageParseNode,
        .create_id_lookup = &_extCreateIdLookup};
};
}  // namespace mongo::extension::host_connector
