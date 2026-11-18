// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/extension/public/api.h"
#include "mongo/db/extension/sdk/assert_util.h"
#include "mongo/db/extension/shared/byte_buf.h"
#include "mongo/db/extension/shared/byte_buf_utils.h"
#include "mongo/db/extension/shared/extension_status.h"
#include "mongo/util/modules.h"

#include <memory>
#include <string>
#include <string_view>

namespace mongo::extension::sdk {

/**
 * CommandDescriptor is the base class for implementing the ::MongoExtensionCommandDescriptor
 * interface by an extension.
 *
 * An extension command must provide a specialization of this base class, and expose it to the
 * host as an ExtensionCommandDescriptorAdapter.
 */
class CommandDescriptor {
public:
    virtual ~CommandDescriptor() = default;

    std::string_view getName() const {
        return std::string_view(_name);
    }

    /**
     * Returns a BSON document containing the command's static properties, which must conform to
     * the MongoExtensionCommandStaticProperties IDL schema.
     */
    virtual BSONObj getProperties() const = 0;

    /**
     * Executes the command. 'requestBson' contains the full command request and 'ctx' provides
     * the per-invocation command context, which is only valid for the duration of this call.
     *
     * Returns the command response as a BSON document.
     */
    virtual BSONObj run(const BSONObj& requestBson,
                        const ::MongoExtensionCommandContext& ctx) const = 0;

protected:
    CommandDescriptor(std::string name) : _name(std::move(name)) {}
    CommandDescriptor() = delete;

    const std::string _name;
};

/**
 * ExtensionCommandDescriptorAdapter is a boundary object representation of a
 * ::MongoExtensionCommandDescriptor. It is meant to abstract away the C++ implementation by the
 * extension and provides the interface at the API boundary which will be called upon by the host.
 * The static VTABLE member points to static methods which ensure the correct conversion from C++
 * context to the C API context.
 *
 * This abstraction is required to ensure we maintain the public ::MongoExtensionCommandDescriptor
 * interface and layout as dictated by the public API. Any polymorphic behavior must be deferred to
 * and implemented by the CommandDescriptor.
 */
class ExtensionCommandDescriptorAdapter final : public ::MongoExtensionCommandDescriptor {
public:
    ExtensionCommandDescriptorAdapter(std::unique_ptr<CommandDescriptor> descriptor)
        : ::MongoExtensionCommandDescriptor(&VTABLE), _descriptor(std::move(descriptor)) {
        sdk_tassert(13729900, "Provided CommandDescriptor is null", _descriptor != nullptr);
    }

    ~ExtensionCommandDescriptorAdapter() = default;

    ExtensionCommandDescriptorAdapter(const ExtensionCommandDescriptorAdapter&) = delete;
    ExtensionCommandDescriptorAdapter& operator=(const ExtensionCommandDescriptorAdapter&) = delete;
    ExtensionCommandDescriptorAdapter(ExtensionCommandDescriptorAdapter&&) = delete;
    ExtensionCommandDescriptorAdapter& operator=(ExtensionCommandDescriptorAdapter&&) = delete;

private:
    const CommandDescriptor& getImpl() const noexcept {
        return *_descriptor;
    }

    static ::MongoExtensionByteView _extGetName(
        const ::MongoExtensionCommandDescriptor* descriptor) noexcept {
        return stringViewAsByteView(
            static_cast<const ExtensionCommandDescriptorAdapter*>(descriptor)->getImpl().getName());
    }

    static ::MongoExtensionStatus* _extGetProperties(
        const ::MongoExtensionCommandDescriptor* descriptor,
        ::MongoExtensionByteBuf** properties) noexcept {
        return wrapCXXAndConvertExceptionToStatus([&]() {
            *properties = nullptr;
            const auto& impl =
                static_cast<const ExtensionCommandDescriptorAdapter*>(descriptor)->getImpl();
            *properties = new ByteBuf(impl.getProperties());
        });
    }

    static ::MongoExtensionStatus* _extRun(const ::MongoExtensionCommandDescriptor* descriptor,
                                           ::MongoExtensionByteView requestBson,
                                           const ::MongoExtensionCommandContext* ctx,
                                           ::MongoExtensionByteBuf** responseBson) noexcept {
        return wrapCXXAndConvertExceptionToStatus([&]() {
            *responseBson = nullptr;
            sdk_tassert(13729901, "Command context was null", ctx != nullptr);
            const auto& impl =
                static_cast<const ExtensionCommandDescriptorAdapter*>(descriptor)->getImpl();
            *responseBson = new ByteBuf(impl.run(bsonObjFromByteView(requestBson), *ctx));
        });
    }

    static constexpr ::MongoExtensionCommandDescriptorVTable VTABLE = {
        .get_name = &_extGetName,
        .get_properties = &_extGetProperties,
        .run = &_extRun,
    };

    std::unique_ptr<CommandDescriptor> _descriptor;
};

}  // namespace mongo::extension::sdk
