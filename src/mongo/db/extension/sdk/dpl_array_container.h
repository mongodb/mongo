// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once
#include "mongo/db/extension/public/api.h"
#include "mongo/db/extension/sdk/assert_util.h"
#include "mongo/db/extension/sdk/raii_vector_to_abi_array.h"
#include "mongo/db/extension/shared/handle/aggregation_stage/parse_node.h"
#include "mongo/util/modules.h"

namespace mongo::extension::sdk {

/**
 * DPLArrayContainer stores a vector of VariantDPLHandle elements that can be
 * transferred into a pre-allocated MongoExtensionDPLArray.
 */
class DPLArrayContainer {
public:
    DPLArrayContainer(std::vector<VariantDPLHandle> elements) : _elements(std::move(elements)) {}

    /**
     * Returns the number of elements in the DPLArrayContainer.
     */
    size_t size() const {
        return _elements.size();
    }

    /**
     * Transfers ownership of the elements in the container into a pre-allocated
     * ::MongoExtensionDPLArray. The caller must pre-allocate the array with the correct size.
     */
    void transfer(::MongoExtensionDPLArray& array) {
        sdk_tassert(11368303,
                    (str::stream()
                     << "Provided MongoExtensionDPLArray size must match container size: got "
                     << array.size << ", but required " << size()),
                    array.size == size());
        raiiVectorToAbiArray(std::move(_elements), array);
    }

private:
    std::vector<VariantDPLHandle> _elements;
};

/**
 * ExtensionDPLArrayContainerAdapter is a boundary object representation of a
 * ::MongoExtensionDPLArrayContainer. It is meant to abstract away the C++ implementation
 * by the extension and provides the interface at the API boundary which will be called upon by the
 * host. The static VTABLE member points to static methods which ensure the correct conversion from
 * C++ context to the C API context.
 */
class ExtensionDPLArrayContainerAdapter final : public ::MongoExtensionDPLArrayContainer {
public:
    ExtensionDPLArrayContainerAdapter(DPLArrayContainer&& container)
        : ::MongoExtensionDPLArrayContainer{&VTABLE}, _container(std::move(container)) {}

    ExtensionDPLArrayContainerAdapter(const ExtensionDPLArrayContainerAdapter&) = delete;
    ExtensionDPLArrayContainerAdapter& operator=(const ExtensionDPLArrayContainerAdapter&) = delete;
    ExtensionDPLArrayContainerAdapter(ExtensionDPLArrayContainerAdapter&&) = delete;
    ExtensionDPLArrayContainerAdapter& operator=(ExtensionDPLArrayContainerAdapter&&) = delete;

    static ::MongoExtensionDPLArrayContainerVTable getVTable() {
        return VTABLE;
    }

private:
    const DPLArrayContainer& getImpl() const noexcept {
        return _container;
    }

    DPLArrayContainer& getImpl() noexcept {
        return _container;
    }

    static void _extDestroy(::MongoExtensionDPLArrayContainer* container) noexcept {
        delete static_cast<ExtensionDPLArrayContainerAdapter*>(container);
    }

    static size_t _extSize(const ::MongoExtensionDPLArrayContainer* container) noexcept {
        return static_cast<const ExtensionDPLArrayContainerAdapter*>(container)->getImpl().size();
    }

    static ::MongoExtensionStatus* _extTransfer(::MongoExtensionDPLArrayContainer* container,
                                                ::MongoExtensionDPLArray* array) noexcept {
        return wrapCXXAndConvertExceptionToStatus([&]() {
            sdk_tassert(11368300, "Provided MongoExtensionDPLArray is null", array != nullptr);
            auto* adapter = static_cast<ExtensionDPLArrayContainerAdapter*>(container);
            adapter->getImpl().transfer(*array);
        });
    }

    static constexpr ::MongoExtensionDPLArrayContainerVTable VTABLE = {
        .destroy = &_extDestroy, .size = &_extSize, .transfer = &_extTransfer};
    DPLArrayContainer _container;
};

}  // namespace mongo::extension::sdk

