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
#include "mongo/db/extension/sdk/aggregation_stage.h"
#include "mongo/db/extension/sdk/extension_status.h"
#include "mongo/db/extension/sdk/host_portal.h"

#include <memory>

namespace mongo::extension::sdk {

/**
 * To register a new extension, extension writers should derive from this class and implement
 * initialize() to register aggregation stages. See test_examples/foo.cpp for an example.
 */
class Extension {
public:
    virtual ~Extension() = default;

    virtual void initialize(const HostPortalHandle& portal) = 0;

protected:
    template <class StageDescriptor>
    void _registerStage(const HostPortalHandle& portal) {
        // Error out if StageDescriptor is already registered to this extension.
        uassert(10696402,
                str::stream() << StageDescriptor::kStageName << " is already registered",
                _stageDescriptors.find(StageDescriptor::kStageName) == _stageDescriptors.end());

        auto stageDesc = std::make_unique<ExtensionAggregationStageDescriptor>(
            std::make_unique<StageDescriptor>());

        portal.registerStageDescriptor(stageDesc.get());

        _stageDescriptors.emplace(StageDescriptor::kStageName, std::move(stageDesc));
    }

private:
    stdx::unordered_map<std::string, std::unique_ptr<ExtensionAggregationStageDescriptor>>
        _stageDescriptors;
};

/**
 * Adapter from a C++ MongoExtension to the C API MongoExtension struct.
 */
class ExtensionAdapter final : public ::MongoExtension {
public:
    ExtensionAdapter(std::unique_ptr<sdk::Extension> extensionPointer,
                     ::MongoExtensionAPIVersion version)
        : ::MongoExtension{&VTABLE, version}, _extensionPointer(std::move(extensionPointer)) {}

    ~ExtensionAdapter() = default;

private:
    static ::MongoExtensionStatus* _extInitialize(
        const ::MongoExtension* extensionPtr, const ::MongoExtensionHostPortal* portal) noexcept {
        return enterCXX([&]() {
            auto hostPortal = HostPortalHandle(portal);
            static_cast<const sdk::ExtensionAdapter*>(extensionPtr)
                ->_extensionPointer->initialize(hostPortal);
        });
    }
    static constexpr ::MongoExtensionVTable VTABLE{&_extInitialize};
    std::unique_ptr<sdk::Extension> _extensionPointer;
};

/**
 * Base case macro to define get_mongodb_extension.
 */
#define REGISTER_EXTENSION_WITH_VERSION(MyExtensionType, apiVersion)                         \
    extern "C" {                                                                             \
    ::MongoExtensionStatus* get_mongodb_extension(                                           \
        const ::MongoExtensionAPIVersionVector* hostVersions,                                \
        const ::MongoExtension** extension) {                                                \
        return mongo::extension::sdk::enterCXX([&] {                                         \
            static auto wrapper = std::make_unique<mongo::extension::sdk::ExtensionAdapter>( \
                std::make_unique<MyExtensionType>(), (apiVersion));                          \
            *extension = reinterpret_cast<const ::MongoExtension*>(wrapper.get());           \
        });                                                                                  \
    }                                                                                        \
    }

/**
 * Default case. Registers MyExtension with MONGODB_EXTENSION_API_VERSION.
 */
#define REGISTER_EXTENSION(MyExtensionType) \
    REGISTER_EXTENSION_WITH_VERSION(MyExtensionType, (MONGODB_EXTENSION_API_VERSION))

}  // namespace mongo::extension::sdk
