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
#include "mongo/db/extension/sdk/assert_util.h"
#include "mongo/db/extension/shared/byte_buf.h"
#include "mongo/db/extension/shared/extension_status.h"
#include "mongo/util/modules.h"

#include <memory>
#include <string>
#include <string_view>

namespace mongo::extension::sdk {

/**
 * LogicalAggStage is the base class for implementing the
 * ::MongoExtensionLogicalAggStage interface by an extension.
 *
 * An extension must provide a specialization of this base class, and
 * expose it to the host as a ExtensionLogicalAggStage.
 */
class LogicalAggStage {
public:
    LogicalAggStage() = default;
    virtual BSONObj serialize() const = 0;
    virtual BSONObj explain(::MongoExtensionExplainVerbosity verbosity) const = 0;
    virtual ~LogicalAggStage() = default;
};

/**
 * ExtensionLogicalAggStage is a boundary object representation of a
 * ::MongoExtensionLogicalAggStage. It is meant to abstract away the C++ implementation
 * by the extension and provides the interface at the API boundary which will be called upon by the
 * host. The static VTABLE member points to static methods which ensure the correct conversion from
 * C++ context to the C API context.
 *
 * This abstraction is required to ensure we maintain the public
 * ::MongoExtensionLogicalAggStage interface and layout as dictacted by the public API.
 * Any polymorphic behavior must be deferred to and implemented by the underlying
 * LogicalAggStage.
 */
class ExtensionLogicalAggStage final : public ::MongoExtensionLogicalAggStage {
public:
    ExtensionLogicalAggStage(std::unique_ptr<LogicalAggStage> logicalStage)
        : ::MongoExtensionLogicalAggStage{&VTABLE}, _stage(std::move(logicalStage)) {}
    ~ExtensionLogicalAggStage() = default;

    const LogicalAggStage& getImpl() const noexcept {
        return *_stage;
    }

private:
    static void _extDestroy(::MongoExtensionLogicalAggStage* extlogicalStage) noexcept {
        delete static_cast<ExtensionLogicalAggStage*>(extlogicalStage);
    }

    static MongoExtensionStatus* _extSerialize(
        const ::MongoExtensionLogicalAggStage* extLogicalStage,
        ::MongoExtensionByteBuf** output) noexcept {
        return wrapCXXAndConvertExceptionToStatus([&]() {
            *output = nullptr;

            const auto& impl =
                static_cast<const ExtensionLogicalAggStage*>(extLogicalStage)->getImpl();

            *output = new VecByteBuf(impl.serialize());
        });
    }

    static ::MongoExtensionStatus* _extExplain(
        const ::MongoExtensionLogicalAggStage* extLogicalStage,
        ::MongoExtensionExplainVerbosity verbosity,
        ::MongoExtensionByteBuf** output) noexcept {
        return wrapCXXAndConvertExceptionToStatus([&]() {
            *output = nullptr;

            const auto& impl =
                static_cast<const ExtensionLogicalAggStage*>(extLogicalStage)->getImpl();

            // Allocate a buffer on the heap. Ownership is transferred to the caller.
            *output = new VecByteBuf(impl.explain(verbosity));
        });
    };

    static constexpr ::MongoExtensionLogicalAggStageVTable VTABLE = {
        .destroy = &_extDestroy, .serialize = &_extSerialize, .explain = &_extExplain};
    std::unique_ptr<LogicalAggStage> _stage;
};

/**
 * AggStageAstNode is the base class for implementing the
 * ::MongoExtensionAggStageAstNode interface by an extension.
 *
 * An extension must provide a specialization of this base class, and
 * expose it to the host as a ExtensionAggStageAstNode.
 */
class AggStageAstNode {
public:
    AggStageAstNode() = default;
    virtual ~AggStageAstNode() = default;


    std::string_view getName() const {
        return _name;
    }

    virtual std::unique_ptr<LogicalAggStage> bind() const = 0;

protected:
    AggStageAstNode(std::string_view name) : _name(name) {}

private:
    const std::string_view _name;
};

/**
 * ExtensionAggStageAstNode is a boundary object representation of a
 * ::MongoExtensionAggStageAstNode. It is meant to abstract away the C++ implementation
 * by the extension and provides the interface at the API boundary which will be called upon by the
 * host. The static VTABLE member points to static methods which ensure the correct conversion from
 * C++ context to the C API context.
 *
 * This abstraction is required to ensure we maintain the public
 * ::MongoExtensionAggStageAstNode interface and layout as dictated by the public API.
 * Any polymorphic behavior must be deferred to and implemented by the underlying
 * AggStageAstNode.
 */
class ExtensionAggStageAstNode final : public ::MongoExtensionAggStageAstNode {
public:
    ExtensionAggStageAstNode(std::unique_ptr<AggStageAstNode> astNode)
        : ::MongoExtensionAggStageAstNode{&VTABLE}, _astNode(std::move(astNode)) {}
    ~ExtensionAggStageAstNode() = default;

private:
    const AggStageAstNode& getImpl() const noexcept {
        return *_astNode;
    }

    AggStageAstNode& getImpl() noexcept {
        return *_astNode;
    }

    static void _extDestroy(::MongoExtensionAggStageAstNode* extAstNode) noexcept {
        delete static_cast<ExtensionAggStageAstNode*>(extAstNode);
    }

    static ::MongoExtensionByteView _extGetName(
        const ::MongoExtensionAggStageAstNode* astNode) noexcept {
        return stringViewAsByteView(
            static_cast<const ExtensionAggStageAstNode*>(astNode)->getImpl().getName());
    }

    static ::MongoExtensionStatus* _extBind(
        const ::MongoExtensionAggStageAstNode* astNode,
        ::MongoExtensionLogicalAggStage** logicalStage) noexcept {
        return wrapCXXAndConvertExceptionToStatus([&]() {
            auto logicalStagePtr =
                static_cast<const ExtensionAggStageAstNode*>(astNode)->getImpl().bind();

            *logicalStage = new ExtensionLogicalAggStage(std::move(logicalStagePtr));
        });
    }

    static constexpr ::MongoExtensionAggStageAstNodeVTable VTABLE = {
        .destroy = &_extDestroy, .get_name = &_extGetName, .bind = &_extBind};
    std::unique_ptr<AggStageAstNode> _astNode;
};

/**
 * Represents the possible types of nodes created during expansion.
 *
 * Expansion can result in four types of nodes:
 * 1. Host-defined parse node
 * 2. Extension-defined parse node
 * 3. Host-defined AST node
 * 4. Extension-defined AST node
 *
 * This variant allows extension developers to return both host- and extension-defined nodes in
 * AggStageParseNode::expand() without knowing the underlying implementation of host-defined
 * nodes.
 *
 * The host is responsible for differentiating between host- and extension-defined nodes later on.
 */
using VariantNode =
    std::variant<::MongoExtensionAggStageParseNode*, ::MongoExtensionAggStageAstNode*>;

/**
 * AggStageParseNode is the base class for implementing the
 * ::MongoExtensionAggStageParseNode interface by an extension.
 *
 * An extension aggregation stage parse node must provide a specialization of this base class and
 * expose it to the host as an ExtensionAggStageParseNode.
 */
class AggStageParseNode {
public:
    virtual ~AggStageParseNode() = default;

    std::string_view getName() const {
        return _name;
    }

    virtual BSONObj getQueryShape(const ::MongoExtensionHostQueryShapeOpts* ctx) const = 0;

    virtual size_t getExpandedSize() const = 0;

    virtual std::vector<VariantNode> expand() const = 0;

protected:
    AggStageParseNode(std::string_view name) : _name(name) {}

private:
    const std::string_view _name;
};

/**
 * ExtensionAggStageParseNode is a boundary object representation of a
 * ::MongoExtensionAggStageParseNode. It is meant to abstract away the C++ implementation by
 * the extension and provides the interface at the API boundary which will be called upon by the
 * host. The static VTABLE member points to static methods which ensure the correct conversion from
 * C++ context to the C API context.
 *
 * This abstraction is required to ensure we maintain the public
 * ::MongoExtensionAggStageParseNode interface and layout as dictated by the public API.
 * Any polymorphic behavior must be deferred to and implemented by the AggStageParseNode.
 */
class ExtensionAggStageParseNode final : public ::MongoExtensionAggStageParseNode {
public:
    ExtensionAggStageParseNode(std::unique_ptr<AggStageParseNode> parseNode)
        : ::MongoExtensionAggStageParseNode(&VTABLE), _parseNode(std::move(parseNode)) {}

    ~ExtensionAggStageParseNode() = default;

private:
    const AggStageParseNode& getImpl() const noexcept {
        return *_parseNode;
    }

    AggStageParseNode& getImpl() noexcept {
        return *_parseNode;
    }

    static void _extDestroy(::MongoExtensionAggStageParseNode* parseNode) noexcept {
        delete static_cast<ExtensionAggStageParseNode*>(parseNode);
    }

    static ::MongoExtensionByteView _extGetName(
        const ::MongoExtensionAggStageParseNode* parseNode) noexcept {
        return stringViewAsByteView(
            static_cast<const ExtensionAggStageParseNode*>(parseNode)->getImpl().getName());
    }

    static ::MongoExtensionStatus* _extGetQueryShape(
        const ::MongoExtensionAggStageParseNode* parseNode,
        const ::MongoExtensionHostQueryShapeOpts* ctx,
        ::MongoExtensionByteBuf** queryShape) noexcept {
        return wrapCXXAndConvertExceptionToStatus([&]() {
            *queryShape = nullptr;

            const auto& impl = static_cast<const ExtensionAggStageParseNode*>(parseNode)->getImpl();

            // Allocate a buffer on the heap. Ownership is transferred to the caller.
            *queryShape = new VecByteBuf(impl.getQueryShape(ctx));
        });
    };

    static size_t _extGetExpandedSize(const ::MongoExtensionAggStageParseNode* parseNode) noexcept {
        return static_cast<const ExtensionAggStageParseNode*>(parseNode)
            ->getImpl()
            .getExpandedSize();
    }

    /**
     * Converts an SDK VariantNode into a tagged union of ABI objects and writes the raw pointers
     * into the host-allocated ExpandedArray element.
     */
    struct ConsumeVariantNodeToAbi {
        ::MongoExtensionExpandedArrayElement& dst;

        void operator()(::MongoExtensionAggStageParseNode* parseNode) const {
            dst.type = kParseNode;
            dst.parse = parseNode;
        }

        void operator()(::MongoExtensionAggStageAstNode* astNode) const {
            dst.type = kAstNode;
            dst.ast = astNode;
        }
    };

    /*
     * Invokes the destructor for a ::MongoExtensionAggStageParseNode and sets the element
     * to null.
     */
    static void destroyAbiNode(::MongoExtensionAggStageParseNode*& node) noexcept {
        if (node && node->vtable && node->vtable->destroy) {
            node->vtable->destroy(node);
        }
        node = nullptr;
    }

    /*
     * Invokes the destructor for a ::MongoExtensionAggStageAstNode and sets the element
     * to null.
     */
    static void destroyAbiNode(::MongoExtensionAggStageAstNode*& node) noexcept {
        if (node && node->vtable && node->vtable->destroy) {
            node->vtable->destroy(node);
        }
        node = nullptr;
    }

    /*
     * Invokes the destructor for a MongoExtensionExpandedArrayElement and sets the element to null.
     */
    static void destroyArrayElement(MongoExtensionExpandedArrayElement& node) noexcept {
        switch (node.type) {
            case kParseNode: {
                destroyAbiNode(node.parse);
                break;
            }
            case kAstNode: {
                destroyAbiNode(node.ast);
                break;
            }
            default: {
                // Memory is leaked if the type tag is invalid, but this only happens if the
                // extension violates the API contract.
                break;
            }
        }
    }

    /**
     * Expands the provided parse node into one or more AST or parse nodes.
     *
     * The resultant expanded array is allocated by the caller and populated by this function. If
     * populating the expanded array fails for any reason, the expanded array is cleared.
     *
     * The caller is responsible for freeing any memory used by the extension during `expand()`.
     */
    static ::MongoExtensionStatus* _extExpand(const ::MongoExtensionAggStageParseNode* parseNode,
                                              ::MongoExtensionExpandedArray* expanded) noexcept;

    static constexpr ::MongoExtensionAggStageParseNodeVTable VTABLE = {
        .destroy = &_extDestroy,
        .get_name = &_extGetName,
        .get_query_shape = &_extGetQueryShape,
        .get_expanded_size = &_extGetExpandedSize,
        .expand = &_extExpand};
    std::unique_ptr<AggStageParseNode> _parseNode;
};

/**
 * AggStageDescriptor is the base class for implementing the
 * ::MongoExtensionAggStageDescriptor interface by an extension.
 *
 * An extension aggregation stage descriptor must provide a specialization of this base class, and
 * expose it to the host as an ExtensionAggStageDescriptor.
 */
class AggStageDescriptor {
public:
    virtual ~AggStageDescriptor() = default;

    std::string_view getName() const {
        return std::string_view(_name);
    }

    ::MongoExtensionAggStageType getType() const {
        return _type;
    }

    virtual std::unique_ptr<class AggStageParseNode> parse(BSONObj stageBson) const = 0;

protected:
    AggStageDescriptor(std::string name, ::MongoExtensionAggStageType type)
        : _name(std::move(name)), _type(type) {}

    const std::string _name;
    ::MongoExtensionAggStageType _type;
};

/**
 * ExtensionAggStageDescriptor is a boundary object representation of a
 * ::MongoExtensionAggStageDescriptor. It is meant to abstract away the C++ implementation
 * by the extension and provides the interface at the API boundary which will be called upon by the
 * host. The static VTABLE member points to static methods which ensure the correct conversion from
 * C++ context to the C API context.
 *
 * This abstraction is required to ensure we maintain the public
 * ::MongoExtensionAggStageDescriptor interface and layout as dictated by the public API.
 * Any polymorphic behavior must be deferred to and implemented by the AggStageDescriptor.
 */
class ExtensionAggStageDescriptor final : public ::MongoExtensionAggStageDescriptor {
public:
    ExtensionAggStageDescriptor(std::unique_ptr<AggStageDescriptor> descriptor)
        : ::MongoExtensionAggStageDescriptor(&VTABLE), _descriptor(std::move(descriptor)) {}

    ~ExtensionAggStageDescriptor() = default;

private:
    const AggStageDescriptor& getImpl() const noexcept {
        return *_descriptor;
    }

    AggStageDescriptor& getImpl() noexcept {
        return *_descriptor;
    }

    static ::MongoExtensionByteView _extGetName(
        const ::MongoExtensionAggStageDescriptor* descriptor) noexcept {
        return stringViewAsByteView(
            static_cast<const ExtensionAggStageDescriptor*>(descriptor)->getImpl().getName());
    }

    static ::MongoExtensionAggStageType _extGetType(
        const ::MongoExtensionAggStageDescriptor* descriptor) noexcept {
        return static_cast<const ExtensionAggStageDescriptor*>(descriptor)->getImpl().getType();
    }

    static ::MongoExtensionStatus* _extParse(
        const ::MongoExtensionAggStageDescriptor* descriptor,
        ::MongoExtensionByteView stageBson,
        ::MongoExtensionAggStageParseNode** parseNode) noexcept {
        return wrapCXXAndConvertExceptionToStatus([&]() {
            const auto& impl =
                static_cast<const ExtensionAggStageDescriptor*>(descriptor)->getImpl();
            auto parseNodePtr = impl.parse(bsonObjFromByteView(stageBson));

            tripwireAssert(11217602,
                           (str::stream()
                            << "Descriptor and parse node stage names differ: descriptor='"
                            << std::string(impl.getName()) << "' parseNode='"
                            << std::string(parseNodePtr->getName()) << "'."),
                           impl.getName() == parseNodePtr->getName());

            *parseNode = new ExtensionAggStageParseNode(std::move(parseNodePtr));
        });
    }

    static constexpr ::MongoExtensionAggStageDescriptorVTable VTABLE = {
        .get_type = &_extGetType, .get_name = &_extGetName, .parse = &_extParse};

    std::unique_ptr<AggStageDescriptor> _descriptor;
};

}  // namespace mongo::extension::sdk
