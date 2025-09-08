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
#include "mongo/db/extension/sdk/byte_buf.h"
#include "mongo/db/extension/sdk/extension_status.h"

#include <memory>
#include <string>
#include <string_view>

namespace mongo::extension::sdk {
/**
 * LogicalAggregationStage is the base class for implementing the
 * ::MongoExtensionLogicalAggregationStage interface by an extension.
 *
 * An extension must provide a specialization of this base class, and
 * expose it to the host as a ExtensionLogicalAggregationStage.
 */
class LogicalAggregationStage {
public:
    LogicalAggregationStage() = default;
    virtual ~LogicalAggregationStage() = default;
};

/**
 * ExtensionLogicalAggregationStage is a boundary object representation of a
 * ::MongoExtensionLogicalAggregationStage. It is meant to abstract away the C++ implementation
 * by the extension, and provides the interface at the API boundary which will be called upon by the
 * host. The static VTABLE member points to static methods which ensure the correct conversion from
 * C++ context to the C API context.
 *
 * This abstraction is required to ensure we maintain the public
 * ::MongoExtensionLogicalAggregationStage interface and layout as dictacted by the public API.
 * Any polymorphic bevahiour must be deferred to and implemented by the underlying
 * LogicalAggregationStage.
 */
class ExtensionLogicalAggregationStage final : public ::MongoExtensionLogicalAggregationStage {
public:
    ExtensionLogicalAggregationStage(std::unique_ptr<LogicalAggregationStage> logicalStage)
        : ::MongoExtensionLogicalAggregationStage{&VTABLE}, _stage(std::move(logicalStage)) {}
    ~ExtensionLogicalAggregationStage() = default;

private:
    static void _extDestroy(::MongoExtensionLogicalAggregationStage* extlogicalStage) noexcept {
        delete static_cast<extension::sdk::ExtensionLogicalAggregationStage*>(extlogicalStage);
    }

    static const ::MongoExtensionLogicalAggregationStageVTable VTABLE;
    std::unique_ptr<LogicalAggregationStage> _stage;
};

/**
 * AggregationStageDescriptor is the base class for implementing the
 * ::MongoExtensionAggregationStageDescriptor interface by an extension.
 *
 * An extension aggregation stage descriptor must provide a specialization of this base class, and
 * expose it to the host as a ExtensionAggregationStageDescriptor.
 */
class AggregationStageDescriptor {
public:
    virtual ~AggregationStageDescriptor() = default;

    std::string_view getName() const {
        return std::string_view(_name);
    }

    ::MongoExtensionAggregationStageType getType() const {
        return _type;
    }

    virtual std::unique_ptr<class LogicalAggregationStage> parse(BSONObj stageBson) const = 0;

    // This function should only be called on a kDesugar descriptor and must be overridden by
    // kDesugar stages. If the stage desugars into nothing, return an empty BSONArray.
    virtual BSONArray expand() const {
        tasserted(11038200, "expand() must be overridden for kDesugar stages.");
    }

protected:
    AggregationStageDescriptor(std::string name, ::MongoExtensionAggregationStageType type)
        : _name(std::move(name)), _type(type) {}

    std::string _name;
    ::MongoExtensionAggregationStageType _type;
};

/**
 * ExtensionAggregationStageDescriptor is a boundary object representation of a
 * ::MongoExtensionAggregationStageDescriptor. It is meant to abstract away the C++ implementation
 * by the extension, and provides the interface at the API boundary which will be called upon by the
 * host. The static VTABLE member points to static methods which ensure the correct conversion from
 * C++ context to the C API context.
 *
 * This abstraction is required to ensure we maintain the public
 * ::MongoExtensionAggregationStageDescriptor interface and layout as dictacted by the public API.
 * Any polymorphic bevahiour must be deferred to and implemented by the AggregationStageDescriptor.
 */
class ExtensionAggregationStageDescriptor final
    : public ::MongoExtensionAggregationStageDescriptor {
public:
    ExtensionAggregationStageDescriptor(std::unique_ptr<AggregationStageDescriptor> descriptor)
        : ::MongoExtensionAggregationStageDescriptor(&VTABLE), _descriptor(std::move(descriptor)) {}

    ~ExtensionAggregationStageDescriptor() = default;

    const AggregationStageDescriptor& getImpl() const {
        return *_descriptor;
    }

    AggregationStageDescriptor& getImpl() {
        return *_descriptor;
    }

private:
    static ::MongoExtensionByteView _extGetName(
        const ::MongoExtensionAggregationStageDescriptor* descriptor) noexcept {
        return stringViewAsByteView(
            static_cast<const extension::sdk::ExtensionAggregationStageDescriptor*>(descriptor)
                ->getImpl()
                .getName());
    }

    static ::MongoExtensionAggregationStageType _extGetType(
        const ::MongoExtensionAggregationStageDescriptor* descriptor) noexcept {
        return static_cast<const extension::sdk::ExtensionAggregationStageDescriptor*>(descriptor)
            ->getImpl()
            .getType();
    }

    static ::MongoExtensionStatus* _extParse(
        const ::MongoExtensionAggregationStageDescriptor* descriptor,
        ::MongoExtensionByteView stageBson,
        ::MongoExtensionLogicalAggregationStage** logicalStage) noexcept {
        return extension::sdk::enterCXX([&]() {
            auto logicalStagePtr =
                static_cast<const extension::sdk::ExtensionAggregationStageDescriptor*>(descriptor)
                    ->getImpl()
                    .parse(bsonObjFromByteView(stageBson));

            *logicalStage =
                std::make_unique<mongo::extension::sdk::ExtensionLogicalAggregationStage>(
                    std::move(logicalStagePtr))
                    .release();
        });
    }

    static ::MongoExtensionStatus* _extExpand(
        const ::MongoExtensionAggregationStageDescriptor* descriptor,
        ::MongoExtensionByteBuf** expandedPipelineBSON) noexcept {
        return extension::sdk::enterCXX([&]() {
            *expandedPipelineBSON = nullptr;

            const auto& impl =
                static_cast<const extension::sdk::ExtensionAggregationStageDescriptor*>(descriptor)
                    ->getImpl();

            tassert(11038201,
                    "Only kDesugar stages can override expand(). It is invalid to call expand() on "
                    "any other stage type.",
                    impl.getType() == ::MongoExtensionAggregationStageType::kDesugar);

            // Only update expandedPipelineBSON with real memory once we have a successful code
            // path.
            auto tmp = std::make_unique<VecByteBuf>(impl.expand());
            *expandedPipelineBSON = tmp.release();
        });
    }

    static const ::MongoExtensionAggregationStageDescriptorVTable VTABLE;
    std::unique_ptr<AggregationStageDescriptor> _descriptor;
};
}  // namespace mongo::extension::sdk
