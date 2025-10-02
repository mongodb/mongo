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
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"
#include "mongo/util/scopeguard.h"

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
        delete static_cast<ExtensionLogicalAggregationStage*>(extlogicalStage);
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
 * ::MongoExtensionAggregationStageDescriptor interface and layout as dictated by the public API.
 * Any polymorphic behavior must be deferred to and implemented by the AggregationStageDescriptor.
 */
class ExtensionAggregationStageDescriptor final
    : public ::MongoExtensionAggregationStageDescriptor {
public:
    ExtensionAggregationStageDescriptor(std::unique_ptr<AggregationStageDescriptor> descriptor)
        : ::MongoExtensionAggregationStageDescriptor(&VTABLE), _descriptor(std::move(descriptor)) {}

    ~ExtensionAggregationStageDescriptor() = default;

private:
    const AggregationStageDescriptor& getImpl() const noexcept {
        return *_descriptor;
    }

    AggregationStageDescriptor& getImpl() noexcept {
        return *_descriptor;
    }

    static ::MongoExtensionByteView _extGetName(
        const ::MongoExtensionAggregationStageDescriptor* descriptor) noexcept {
        return stringViewAsByteView(
            static_cast<const ExtensionAggregationStageDescriptor*>(descriptor)
                ->getImpl()
                .getName());
    }

    static ::MongoExtensionAggregationStageType _extGetType(
        const ::MongoExtensionAggregationStageDescriptor* descriptor) noexcept {
        return static_cast<const ExtensionAggregationStageDescriptor*>(descriptor)
            ->getImpl()
            .getType();
    }

    static ::MongoExtensionStatus* _extParse(
        const ::MongoExtensionAggregationStageDescriptor* descriptor,
        ::MongoExtensionByteView stageBson,
        ::MongoExtensionLogicalAggregationStage** logicalStage) noexcept {
        return enterCXX([&]() {
            auto logicalStagePtr =
                static_cast<const ExtensionAggregationStageDescriptor*>(descriptor)
                    ->getImpl()
                    .parse(bsonObjFromByteView(stageBson));

            *logicalStage =
                std::make_unique<ExtensionLogicalAggregationStage>(std::move(logicalStagePtr))
                    .release();
        });
    }

    static const ::MongoExtensionAggregationStageDescriptorVTable VTABLE;
    std::unique_ptr<AggregationStageDescriptor> _descriptor;
};

/**
 * AggregationStageAstNode is the base class for implementing the
 * ::MongoExtensionAggregationStageAstNode interface by an extension.
 *
 * An extension must provide a specialization of this base class, and
 * expose it to the host as a ExtensionAggregationStageAstNode.
 */
class AggregationStageAstNode {
public:
    AggregationStageAstNode() = default;
    virtual ~AggregationStageAstNode() = default;

    virtual std::unique_ptr<LogicalAggregationStage> bind() const = 0;
};

/**
 * ExtensionAggregationStageAstNode is a boundary object representation of a
 * ::MongoExtensionAggregationStageAstNode. It is meant to abstract away the C++ implementation
 * by the extension, and provides the interface at the API boundary which will be called upon by the
 * host. The static VTABLE member points to static methods which ensure the correct conversion from
 * C++ context to the C API context.
 *
 * This abstraction is required to ensure we maintain the public
 * ::MongoExtensionAggregationStageAstNode interface and layout as dictated by the public API.
 * Any polymorphic behavior must be deferred to and implemented by the underlying
 * AggregationStageAstNode.
 */
class ExtensionAggregationStageAstNode final : public ::MongoExtensionAggregationStageAstNode {
public:
    ExtensionAggregationStageAstNode(std::unique_ptr<AggregationStageAstNode> astNode)
        : ::MongoExtensionAggregationStageAstNode{&VTABLE}, _astNode(std::move(astNode)) {}
    ~ExtensionAggregationStageAstNode() = default;

private:
    const AggregationStageAstNode& getImpl() const noexcept {
        return *_astNode;
    }

    AggregationStageAstNode& getImpl() noexcept {
        return *_astNode;
    }

    static void _extDestroy(::MongoExtensionAggregationStageAstNode* extAstNode) noexcept {
        delete static_cast<ExtensionAggregationStageAstNode*>(extAstNode);
    }

    static ::MongoExtensionStatus* _extBind(
        const ::MongoExtensionAggregationStageAstNode* astNode,
        ::MongoExtensionLogicalAggregationStage** logicalStage) noexcept {
        return enterCXX([&]() {
            auto logicalStagePtr =
                static_cast<const ExtensionAggregationStageAstNode*>(astNode)->getImpl().bind();

            *logicalStage =
                std::make_unique<ExtensionLogicalAggregationStage>(std::move(logicalStagePtr))
                    .release();
        });
    }

    static const MongoExtensionAggregationStageAstNodeVTable VTABLE;
    std::unique_ptr<AggregationStageAstNode> _astNode;
};

class AggregationStageParseNode;
using VariantNode = std::variant<std::unique_ptr<AggregationStageParseNode>,
                                 std::unique_ptr<AggregationStageAstNode>>;

/**
 * AggregationStageParseNode is the base class for implementing the
 * ::MongoExtensionAggregationStageParseNode interface by an extension.
 *
 * An extension aggregation stage parse node must provide a specialization of this base class and
 * expose it to the host as an ExtensionAggregationStageParseNode.
 */
class AggregationStageParseNode {
public:
    virtual ~AggregationStageParseNode() = default;

    // TODO(SERVER-111368): Add getQueryShape().

    virtual size_t getExpandedSize() const = 0;

    virtual std::vector<VariantNode> expand() const = 0;
};

/**
 * ExtensionAggregationStageParseNode is a boundary object representation of a
 * ::MongoExtensionAggregationStageParseNode. It is meant to abstract away the C++ implementation by
 * the extension, and provides the interface at the API boundary which will be called upon by the
 * host. The static VTABLE member points to static methods which ensure the correct conversion from
 * C++ context to the C API context.
 *
 * This abstraction is required to ensure we maintain the public
 * ::MongoExtensionAggregationStageParseNode interface and layout as dictated by the public API.
 * Any polymorphic behavior must be deferred to and implemented by the AggregationStageParseNode.
 */
class ExtensionAggregationStageParseNode final : public ::MongoExtensionAggregationStageParseNode {
public:
    ExtensionAggregationStageParseNode(std::unique_ptr<AggregationStageParseNode> parseNode)
        : ::MongoExtensionAggregationStageParseNode(&VTABLE), _parseNode(std::move(parseNode)) {}

    ~ExtensionAggregationStageParseNode() = default;

private:
    const AggregationStageParseNode& getImpl() const noexcept {
        return *_parseNode;
    }

    AggregationStageParseNode& getImpl() noexcept {
        return *_parseNode;
    }

    static void _extDestroy(::MongoExtensionAggregationStageParseNode* parseNode) noexcept {
        delete static_cast<ExtensionAggregationStageParseNode*>(parseNode);
    }

    static ::MongoExtensionStatus* _extGetQueryShape(
        const ::MongoExtensionAggregationStageParseNode* parseNode,
        ::MongoExtensionByteBuf** queryShape) noexcept {
        // TODO(SERVER-111368): Implement.
        MONGO_UNIMPLEMENTED;
    };

    static size_t _extGetExpandedSize(
        const ::MongoExtensionAggregationStageParseNode* parseNode) noexcept {
        return static_cast<const ExtensionAggregationStageParseNode*>(parseNode)
            ->getImpl()
            .getExpandedSize();
    }

    /**
     * Converts an SDK VariantNode into ABI objects and writes the raw pointers into the
     * host-allocated ExpandedArray element. Ownership stays in the RAII holder vectors until we
     * explicitly release() them.
     */
    struct ConsumeVariantNodeToAbi {
        ::MongoExtensionExpandedArrayElement& dst;
        std::vector<std::unique_ptr<ExtensionAggregationStageParseNode>>& parseNodes;
        std::vector<std::unique_ptr<ExtensionAggregationStageAstNode>>& astNodes;

        void operator()(std::unique_ptr<AggregationStageParseNode>&& parseNode) const {
            parseNodes.push_back(
                std::make_unique<ExtensionAggregationStageParseNode>(std::move(parseNode)));
            dst.type = kParseNode;
            dst.parse = parseNodes.back().get();
        }

        void operator()(std::unique_ptr<AggregationStageAstNode>&& astNode) const {
            astNodes.push_back(
                std::make_unique<ExtensionAggregationStageAstNode>(std::move(astNode)));
            dst.type = kAstNode;
            dst.ast = astNodes.back().get();
        }
    };

    static ::MongoExtensionStatus* _extExpand(
        const ::MongoExtensionAggregationStageParseNode* parseNode,
        ::MongoExtensionExpandedArray* expanded) noexcept {
        return enterCXX([&]() {
            const auto& impl =
                static_cast<const ExtensionAggregationStageParseNode*>(parseNode)->getImpl();
            const auto expandedSize = impl.getExpandedSize();
            tassert(11113801,
                    str::stream() << "MongoExtensionExpandedArray.size must equal required size: "
                                  << "got " << expanded->size << ", but required " << expandedSize,
                    expanded->size == expandedSize);

            auto expandedNodes = impl.expand();
            tassert(11113802,
                    str::stream() << "AggregationStageParseNode expand() returned a different "
                                     "number of elements than getExpandedSize(): returned "
                                  << expandedNodes.size() << ", but required " << expandedSize,
                    expandedNodes.size() == expandedSize);

            // RAII containers for the ABI wrappers, which are owned until we explicitly release().
            std::vector<std::unique_ptr<ExtensionAggregationStageParseNode>> parseNodes;
            std::vector<std::unique_ptr<ExtensionAggregationStageAstNode>> astNodes;
            parseNodes.reserve(expandedSize);
            astNodes.reserve(expandedSize);

            // If we exit early, null any raw pointers written to the caller's buffer. The RAII
            // containers will destroy the ABI wrappers automatically.
            size_t filled = 0;
            ScopeGuard guard([&]() {
                for (size_t i = 0; i < filled; ++i) {
                    auto& el = expanded->elements[i];
                    el.parse = nullptr;
                }
            });

            // Populate the caller's buffer directly with raw pointers to nodes.
            for (size_t i = 0; i < expandedSize; ++i) {
                auto& dst = expanded->elements[i];
                std::visit(ConsumeVariantNodeToAbi{dst, parseNodes, astNodes},
                           std::move(expandedNodes[i]));
                ++filled;
            }
            guard.dismiss();

            // Transfer ownership to the caller by releasing nodes from the RAII containers.
            ([&]() noexcept {
                for (auto& node : parseNodes) {
                    [[maybe_unused]] auto* releasedPtr = node.release();
                }
                for (auto& node : astNodes) {
                    [[maybe_unused]] auto* releasedPtr = node.release();
                }
            })();
        });
    }

    static const ::MongoExtensionAggregationStageParseNodeVTable VTABLE;
    std::unique_ptr<AggregationStageParseNode> _parseNode;
};

}  // namespace mongo::extension::sdk
