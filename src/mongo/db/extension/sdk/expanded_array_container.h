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
 * ExpandedArrayContainer stores a vector of VariantNodeHandle elements that can be
 * transferred into a pre-allocated MongoExtensionExpandedArray.
 */
class ExpandedArrayContainer {
public:
    ExpandedArrayContainer(std::vector<VariantNodeHandle> elements)
        : _elements(std::move(elements)) {}

    /**
     * Returns the number of elements in the ExpandedArrayContainer.
     */
    size_t size() const {
        return _elements.size();
    }

    /**
     * Transfers ownership of the elements in the container into a pre-allocated
     * ::MongoExtensionExpandedArray. The caller must pre-allocate the array with the correct size.
     */
    void transfer(::MongoExtensionExpandedArray& array) {
        sdk_tassert(11591600,
                    (str::stream()
                     << "Provided MongoExtensionExpandedArray size must match container size: got "
                     << array.size << ", but required " << size()),
                    array.size == size());
        raiiVectorToAbiArray(std::move(_elements), array);
    }

private:
    std::vector<VariantNodeHandle> _elements;
};

/**
 * ExtensionExpandedArrayContainerAdapter is a boundary object representation of a
 * ::MongoExtensionExpandedArrayContainer. It is meant to abstract away the C++ implementation
 * by the extension and provides the interface at the API boundary which will be called upon by the
 * host. The static VTABLE member points to static methods which ensure the correct conversion from
 * C++ context to the C API context.
 */
class ExtensionExpandedArrayContainerAdapter final : public ::MongoExtensionExpandedArrayContainer {
public:
    ExtensionExpandedArrayContainerAdapter(ExpandedArrayContainer&& container)
        : ::MongoExtensionExpandedArrayContainer{&VTABLE}, _container(std::move(container)) {}

    ExtensionExpandedArrayContainerAdapter(const ExtensionExpandedArrayContainerAdapter&) = delete;
    ExtensionExpandedArrayContainerAdapter& operator=(
        const ExtensionExpandedArrayContainerAdapter&) = delete;
    ExtensionExpandedArrayContainerAdapter(ExtensionExpandedArrayContainerAdapter&&) = delete;
    ExtensionExpandedArrayContainerAdapter& operator=(ExtensionExpandedArrayContainerAdapter&&) =
        delete;

    static ::MongoExtensionExpandedArrayContainerVTable getVTable() {
        return VTABLE;
    }

private:
    const ExpandedArrayContainer& getImpl() const noexcept {
        return _container;
    }

    ExpandedArrayContainer& getImpl() noexcept {
        return _container;
    }

    static void _extDestroy(::MongoExtensionExpandedArrayContainer* container) noexcept {
        delete static_cast<ExtensionExpandedArrayContainerAdapter*>(container);
    }

    static size_t _extSize(const ::MongoExtensionExpandedArrayContainer* container) noexcept {
        return static_cast<const ExtensionExpandedArrayContainerAdapter*>(container)
            ->getImpl()
            .size();
    }

    static ::MongoExtensionStatus* _extTransfer(::MongoExtensionExpandedArrayContainer* container,
                                                ::MongoExtensionExpandedArray* array) noexcept {
        return wrapCXXAndConvertExceptionToStatus([&]() {
            sdk_tassert(11591601, "Provided MongoExtensionExpandedArray is null", array != nullptr);
            auto* adapter = static_cast<ExtensionExpandedArrayContainerAdapter*>(container);
            adapter->getImpl().transfer(*array);
        });
    }

    static constexpr ::MongoExtensionExpandedArrayContainerVTable VTABLE = {
        .destroy = &_extDestroy, .size = &_extSize, .transfer = &_extTransfer};
    ExpandedArrayContainer _container;
};

}  // namespace mongo::extension::sdk
