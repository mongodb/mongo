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
    ExtensionDPLArrayContainerAdapter(std::unique_ptr<DPLArrayContainer> container)
        : ::MongoExtensionDPLArrayContainer{&VTABLE}, _container(std::move(container)) {
        sdk_tassert(11368304, "Provided DPLArrayContainer is null", _container != nullptr);
    }

private:
    const DPLArrayContainer& getImpl() const noexcept {
        return *_container;
    }

    DPLArrayContainer& getImpl() noexcept {
        return *_container;
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

    static constexpr ::MongoExtensionDPLArrayContainerVTable VTABLE{
        .destroy = &_extDestroy, .size = &_extSize, .transfer = &_extTransfer};
    std::unique_ptr<DPLArrayContainer> _container;
};

}  // namespace mongo::extension::sdk

