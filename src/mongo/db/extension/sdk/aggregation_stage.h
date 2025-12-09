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
#include "mongo/db/extension/sdk/distributed_plan_logic.h"
#include "mongo/db/extension/sdk/operation_metrics_adapter.h"
#include "mongo/db/extension/sdk/query_execution_context_handle.h"
#include "mongo/db/extension/sdk/raii_vector_to_abi_array.h"
#include "mongo/db/extension/shared/byte_buf.h"
#include "mongo/db/extension/shared/extension_status.h"
#include "mongo/db/extension/shared/get_next_result.h"
#include "mongo/db/extension/shared/handle/aggregation_stage/executable_agg_stage.h"
#include "mongo/db/extension/shared/handle/aggregation_stage/parse_node.h"
#include "mongo/util/modules.h"

#include <memory>
#include <string>
#include <string_view>

namespace mongo::extension::sdk {

// Explicit template instantiations are provided in aggregation_stage.cpp.
extern template void raiiVectorToAbiArray<VariantNodeHandle>(
    std::vector<VariantNodeHandle> inputVector, ::MongoExtensionExpandedArray& outputArray);
extern template void raiiVectorToAbiArray<VariantDPLHandle>(
    std::vector<VariantDPLHandle> inputVector, ::MongoExtensionDPLArray& outputArray);

/**
 * LogicalAggStage is the base class for implementing the
 * ::MongoExtensionLogicalAggStage interface by an extension.
 *
 * An extension must provide a specialization of this base class, and
 * expose it to the host as a ExtensionLogicalAggStage.
 */
class ExecAggStageBase;
class DistributedPlanLogic;
class LogicalAggStage {
public:
    virtual ~LogicalAggStage() = default;

    std::string_view getName() const {
        return _name;
    }

    virtual BSONObj serialize() const = 0;
    virtual BSONObj explain(::MongoExtensionExplainVerbosity verbosity) const = 0;
    virtual std::unique_ptr<ExecAggStageBase> compile() const = 0;
    virtual boost::optional<DistributedPlanLogic> getDistributedPlanLogic() const = 0;

protected:
    LogicalAggStage() = delete;  // No default constructor.
    explicit LogicalAggStage(std::string_view name) : _name(name) {}

    const std::string _name;
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
        : ::MongoExtensionLogicalAggStage{&VTABLE}, _stage(std::move(logicalStage)) {
        sdk_tassert(11417105, "Provided LogicalAggStage is null", _stage != nullptr);
    }

    ~ExtensionLogicalAggStage() = default;

    ExtensionLogicalAggStage(const ExtensionLogicalAggStage&) = delete;
    ExtensionLogicalAggStage& operator=(const ExtensionLogicalAggStage&) = delete;
    ExtensionLogicalAggStage(ExtensionLogicalAggStage&&) = delete;
    ExtensionLogicalAggStage& operator=(ExtensionLogicalAggStage&&) = delete;

    const LogicalAggStage& getImpl() const noexcept {
        return *_stage;
    }

private:
    static void _extDestroy(::MongoExtensionLogicalAggStage* extlogicalStage) noexcept {
        delete static_cast<ExtensionLogicalAggStage*>(extlogicalStage);
    }

    static ::MongoExtensionByteView _extGetName(
        const ::MongoExtensionLogicalAggStage* logicalStage) noexcept {
        return stringViewAsByteView(
            static_cast<const ExtensionLogicalAggStage*>(logicalStage)->getImpl().getName());
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

    static ::MongoExtensionStatus* _extCompile(
        const ::MongoExtensionLogicalAggStage* extLogicalStage,
        ::MongoExtensionExecAggStage** output) noexcept;

    static ::MongoExtensionStatus* _extGetDistributedPlanLogic(
        const ::MongoExtensionLogicalAggStage* extLogicalStage,
        ::MongoExtensionDistributedPlanLogic** output) noexcept {
        return wrapCXXAndConvertExceptionToStatus([&]() {
            *output = nullptr;

            const auto& impl =
                static_cast<const ExtensionLogicalAggStage*>(extLogicalStage)->getImpl();

            if (auto dpl = impl.getDistributedPlanLogic()) {
                *output = new ExtensionDistributedPlanLogicAdapter(std::move(*dpl));
            }
        });
    }

    static constexpr ::MongoExtensionLogicalAggStageVTable VTABLE = {
        .destroy = &_extDestroy,
        .get_name = &_extGetName,
        .serialize = &_extSerialize,
        .explain = &_extExplain,
        .compile = &_extCompile,
        .get_distributed_plan_logic = &_extGetDistributedPlanLogic};
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
    virtual ~AggStageAstNode() = default;


    std::string_view getName() const {
        return _name;
    }

    virtual BSONObj getProperties() const {
        return BSONObj();
    }

    virtual std::unique_ptr<LogicalAggStage> bind() const = 0;

protected:
    AggStageAstNode() = delete;  // No default constructor.
    explicit AggStageAstNode(std::string_view name) : _name(name) {}

    const std::string _name;
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
        : ::MongoExtensionAggStageAstNode{&VTABLE}, _astNode(std::move(astNode)) {
        sdk_tassert(11417106, "Provided AggStageAstNode is null", _astNode != nullptr);
    }

    ~ExtensionAggStageAstNode() = default;

    ExtensionAggStageAstNode(const ExtensionAggStageAstNode&) = delete;
    ExtensionAggStageAstNode& operator=(const ExtensionAggStageAstNode&) = delete;
    ExtensionAggStageAstNode(ExtensionAggStageAstNode&&) = delete;
    ExtensionAggStageAstNode& operator=(ExtensionAggStageAstNode&&) = delete;

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

    static ::MongoExtensionStatus* _extGetProperties(
        const ::MongoExtensionAggStageAstNode* astNode,
        ::MongoExtensionByteBuf** properties) noexcept {
        return wrapCXXAndConvertExceptionToStatus([&] {
            *properties = nullptr;

            const auto& impl = static_cast<const ExtensionAggStageAstNode*>(astNode)->getImpl();

            // Allocate a buffer on the heap. Ownership is transferred to the caller.
            *properties = new VecByteBuf(impl.getProperties());
        });
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

    static constexpr ::MongoExtensionAggStageAstNodeVTable VTABLE = {.destroy = &_extDestroy,
                                                                     .get_name = &_extGetName,
                                                                     .get_properties =
                                                                         &_extGetProperties,
                                                                     .bind = &_extBind};
    std::unique_ptr<AggStageAstNode> _astNode;
};

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

    virtual std::vector<VariantNodeHandle> expand() const = 0;

protected:
    AggStageParseNode() = delete;  // No default constructor.
    explicit AggStageParseNode(std::string_view name) : _name(name) {}

    const std::string _name;
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
        : ::MongoExtensionAggStageParseNode(&VTABLE), _parseNode(std::move(parseNode)) {
        sdk_tassert(11417107, "Provided AggStageParseNode is null", _parseNode != nullptr);
    }

    ~ExtensionAggStageParseNode() = default;

    ExtensionAggStageParseNode(const ExtensionAggStageParseNode&) = delete;
    ExtensionAggStageParseNode& operator=(const ExtensionAggStageParseNode&) = delete;
    ExtensionAggStageParseNode(ExtensionAggStageParseNode&&) = delete;
    ExtensionAggStageParseNode& operator=(ExtensionAggStageParseNode&&) = delete;

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

    virtual std::unique_ptr<class AggStageParseNode> parse(BSONObj stageBson) const = 0;

protected:
    AggStageDescriptor() = delete;  // No default constructor.
    explicit AggStageDescriptor(std::string name) : _name(std::move(name)) {}

    const std::string _name;
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
        : ::MongoExtensionAggStageDescriptor(&VTABLE), _descriptor(std::move(descriptor)) {
        sdk_tassert(11417108, "Provided AggStageDescriptor is null", _descriptor != nullptr);
    }

    ~ExtensionAggStageDescriptor() = default;

    ExtensionAggStageDescriptor(const ExtensionAggStageDescriptor&) = delete;
    ExtensionAggStageDescriptor& operator=(const ExtensionAggStageDescriptor&) = delete;
    ExtensionAggStageDescriptor(ExtensionAggStageDescriptor&&) = delete;
    ExtensionAggStageDescriptor& operator=(ExtensionAggStageDescriptor&&) = delete;

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

    static ::MongoExtensionStatus* _extParse(
        const ::MongoExtensionAggStageDescriptor* descriptor,
        ::MongoExtensionByteView stageBson,
        ::MongoExtensionAggStageParseNode** parseNode) noexcept {
        return wrapCXXAndConvertExceptionToStatus([&]() {
            const auto& impl =
                static_cast<const ExtensionAggStageDescriptor*>(descriptor)->getImpl();
            auto parseNodePtr = impl.parse(bsonObjFromByteView(stageBson));

            sdk_tassert(11217602,
                        (str::stream()
                         << "Descriptor and parse node stage names differ: descriptor='"
                         << std::string(impl.getName()) << "' parseNode='"
                         << std::string(parseNodePtr->getName()) << "'."),
                        impl.getName() == parseNodePtr->getName());

            *parseNode = new ExtensionAggStageParseNode(std::move(parseNodePtr));
        });
    }

    static constexpr ::MongoExtensionAggStageDescriptorVTable VTABLE = {.get_name = &_extGetName,
                                                                        .parse = &_extParse};

    std::unique_ptr<AggStageDescriptor> _descriptor;
};

/**
 * ExecAggStage is the base class for implementing the ::MongoExtensionExecAggStage interface by an
 * extension.
 *
 * An extension executable agg stage must provide a specialization of this base class, and
 * expose it to the host as an ExtensionExecAggStage.
 */
class ExecAggStageBase {
public:
    virtual ~ExecAggStageBase() = default;

    virtual ExtensionGetNextResult getNext(const QueryExecutionContextHandle& execCtx,
                                           ::MongoExtensionExecAggStage* execStage) = 0;

    std::string_view getName() const {
        return _name;
    }

    // Extensions are not required to provide metrics if they do not need to.
    virtual std::unique_ptr<OperationMetricsBase> createMetrics() const {
        return nullptr;
    }

    virtual void setSource(UnownedExecAggStageHandle inputStage) = 0;

    virtual void open() = 0;

    virtual void reopen() = 0;

    virtual void close() = 0;

    virtual BSONObj explain(::MongoExtensionExplainVerbosity verbosity) const = 0;

protected:
    ExecAggStageBase(std::string_view name) : _name(name) {}

    virtual UnownedExecAggStageHandle& _getSource() = 0;

    const std::string _name;
};

/**
 * ExecAggStageSource is an execution stage that generates documents. It cannot have a source stage.
 */
class ExecAggStageSource : public ExecAggStageBase {
public:
    void setSource(UnownedExecAggStageHandle inputStage) override {
        sdk_tasserted(10957210, "Calling setSource on a source stage is not supported");
    }

protected:
    ExecAggStageSource(std::string_view name) : ExecAggStageBase(name) {}

    UnownedExecAggStageHandle& _getSource() override {
        sdk_tasserted(10957208, "Calling getSource on a source stage is not supported");
        MONGO_UNREACHABLE;
    }
};

/**
 * ExecAggStageTransform is an execution stage that operates on documents it receives from a
 * predecessor source stage.  It must be provided with a source stage before getNext() is called.
 */
class ExecAggStageTransform : public ExecAggStageBase {
public:
    void setSource(UnownedExecAggStageHandle inputStage) override {
        _inputStage = std::move(inputStage);
    }

protected:
    ExecAggStageTransform(std::string_view name) : ExecAggStageBase(name), _inputStage(nullptr) {}

    UnownedExecAggStageHandle& _getSource() override {
        sdk_tassert(10957209, "Source stage is invalid", _inputStage.isValid());
        return _inputStage;
    }

private:
    UnownedExecAggStageHandle _inputStage;
};

/**
 * ExecAggStage is a boundary object representation of a
 * ::MongoExtensionExecAggStage. It is meant to abstract away the C++ implementation
 * by the extension and provides the interface at the API boundary which will be called upon by the
 * host. The static VTABLE member points to static methods which ensure the correct conversion from
 * C++ context to the C API context.
 *
 * This abstraction is required to ensure we maintain the public
 * ::MongoExtensionExecAggStage interface and layout as dictated by the public API.
 * Any polymorphic behavior must be deferred to and implemented by the ExecAggStage.
 */
class ExtensionExecAggStage final : public ::MongoExtensionExecAggStage {
public:
    ExtensionExecAggStage(std::unique_ptr<ExecAggStageBase> execAggStage)
        : ::MongoExtensionExecAggStage(&VTABLE), _execAggStage(std::move(execAggStage)) {
        sdk_tassert(11417109, "Provided ExecAggStageBase is null", _execAggStage != nullptr);
    }

    ~ExtensionExecAggStage() = default;

    ExtensionExecAggStage(const ExtensionExecAggStage&) = delete;
    ExtensionExecAggStage& operator=(const ExtensionExecAggStage&) = delete;
    ExtensionExecAggStage(ExtensionExecAggStage&&) = delete;
    ExtensionExecAggStage& operator=(ExtensionExecAggStage&&) = delete;

private:
    const ExecAggStageBase& getImpl() const noexcept {
        return *_execAggStage;
    }

    ExecAggStageBase& getImpl() noexcept {
        return *_execAggStage;
    }

    static void _extDestroy(::MongoExtensionExecAggStage* execAggStage) noexcept {
        delete static_cast<ExtensionExecAggStage*>(execAggStage);
    }

    static ::MongoExtensionStatus* _extGetNext(::MongoExtensionExecAggStage* execAggStage,
                                               ::MongoExtensionQueryExecutionContext* execCtxPtr,
                                               ::MongoExtensionGetNextResult* apiResult) noexcept {
        return wrapCXXAndConvertExceptionToStatus([&]() {
            sdk_tassert(11357806, "The api result should be non-null", apiResult != nullptr);
            apiResult->code = ::MongoExtensionGetNextResultCode::kPauseExecution;
            apiResult->resultDocument = createEmptyByteContainer();
            apiResult->resultMetadata = createEmptyByteContainer();

            QueryExecutionContextHandle execCtx{execCtxPtr};

            auto& impl = static_cast<ExtensionExecAggStage*>(execAggStage)->getImpl();
            auto extensionResult = impl.getNext(execCtx, execAggStage);
            extensionResult.toApiResult(*apiResult);
        });
    };

    static ::MongoExtensionByteView _extGetName(
        const ::MongoExtensionExecAggStage* execAggStage) noexcept {
        const auto& impl = static_cast<const ExtensionExecAggStage*>(execAggStage)->getImpl();
        return stringViewAsByteView(impl.getName());
    }

    static ::MongoExtensionStatus* _extCreateMetrics(
        const ::MongoExtensionExecAggStage* execAggStage,
        MongoExtensionOperationMetrics** metrics) noexcept {
        return wrapCXXAndConvertExceptionToStatus([&]() {
            const auto& impl = static_cast<const ExtensionExecAggStage*>(execAggStage)->getImpl();
            auto result = impl.createMetrics();

            auto adapter = new OperationMetricsAdapter(std::move(result));
            *metrics = adapter;
        });
    }

    static ::MongoExtensionStatus* _extSetSource(::MongoExtensionExecAggStage* execAggStage,
                                                 ::MongoExtensionExecAggStage* input) noexcept {
        return wrapCXXAndConvertExceptionToStatus([&]() {
            // This method should only ever be called by an extension transform stage (getImpl()
            // should return an ExecAggStageTransform). Source extension stages, by definition,
            // cannot have their source set. Thus, this method should never be called on a source
            // extension stage. If it is, then the tassert in ExecAggStageSource::setSource(...)
            // will be triggered.
            static_cast<ExtensionExecAggStage*>(execAggStage)
                ->getImpl()
                .setSource(UnownedExecAggStageHandle(input));
        });
    }

    static ::MongoExtensionStatus* _extOpen(::MongoExtensionExecAggStage* execAggStage) noexcept {
        return wrapCXXAndConvertExceptionToStatus(
            [&]() { static_cast<ExtensionExecAggStage*>(execAggStage)->getImpl().open(); });
    }

    static ::MongoExtensionStatus* _extReopen(::MongoExtensionExecAggStage* execAggStage) noexcept {
        return wrapCXXAndConvertExceptionToStatus(
            [&]() { static_cast<ExtensionExecAggStage*>(execAggStage)->getImpl().reopen(); });
    }

    static ::MongoExtensionStatus* _extClose(::MongoExtensionExecAggStage* execAggStage) noexcept {
        return wrapCXXAndConvertExceptionToStatus(
            [&]() { static_cast<ExtensionExecAggStage*>(execAggStage)->getImpl().close(); });
    }

    static ::MongoExtensionStatus* _extExplain(const ::MongoExtensionExecAggStage* execAggStage,
                                               ::MongoExtensionExplainVerbosity verbosity,
                                               ::MongoExtensionByteBuf** output) noexcept {
        return wrapCXXAndConvertExceptionToStatus([&]() {
            *output = nullptr;

            const auto& impl = static_cast<const ExtensionExecAggStage*>(execAggStage)->getImpl();

            // Allocate a buffer on the heap. Ownership is transferred to the caller.
            *output = new VecByteBuf(impl.explain(verbosity));
        });
    };

    static constexpr ::MongoExtensionExecAggStageVTable VTABLE = {.destroy = &_extDestroy,
                                                                  .get_next = &_extGetNext,
                                                                  .get_name = &_extGetName,
                                                                  .create_metrics =
                                                                      &_extCreateMetrics,
                                                                  .set_source = &_extSetSource,
                                                                  .open = &_extOpen,
                                                                  .reopen = &_extReopen,
                                                                  .close = &_extClose,
                                                                  .explain = &_extExplain};
    std::unique_ptr<ExecAggStageBase> _execAggStage;
};

inline ::MongoExtensionStatus* ExtensionLogicalAggStage::_extCompile(
    const ::MongoExtensionLogicalAggStage* extLogicalStage,
    ::MongoExtensionExecAggStage** output) noexcept {
    return wrapCXXAndConvertExceptionToStatus([&]() {
        *output = nullptr;

        const auto& impl = static_cast<const ExtensionLogicalAggStage*>(extLogicalStage)->getImpl();

        *output = new ExtensionExecAggStage(impl.compile());
    });
};

}  // namespace mongo::extension::sdk
