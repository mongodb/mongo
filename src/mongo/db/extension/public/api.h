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

#ifdef __has_include
#if __has_include("mongo/util/modules.h")
#include "mongo/util/modules.h"
#else
#define MONGO_MOD_PUB
#endif
#endif  // __has_include

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Represents the API version of the MongoDB extension, to ensure compatibility between the MongoDB
 * server and the extension.
 *
 * The version is composed of two parts: major and minor. The major version is incremented
 * for incompatible changes and the minor version for for backward-compatible changes.
 */
typedef struct {
    uint32_t major;
    uint32_t minor;
} MongoExtensionAPIVersion;

#define MONGODB_EXTENSION_API_MAJOR_VERSION 0
#define MONGODB_EXTENSION_API_MINOR_VERSION 0

// The current API version of the MongoDB extension.
#define MONGODB_EXTENSION_API_VERSION                                            \
    MongoExtensionAPIVersion {                                                   \
        MONGODB_EXTENSION_API_MAJOR_VERSION, MONGODB_EXTENSION_API_MINOR_VERSION \
    }

/**
 * A generic struct for a vector of Extensions API versions.
 *
 * Used for version compatibility checking, where it contains all supported extensions API versions
 * by the host. For example, if 'versions' contains {v1.3.2, v2.4.3}, then the host will support any
 * extension written for versions in the ranges [v1.0.0, v1.3.2] and [v2.0.0, v2.4.3].
 */
typedef struct {
    uint64_t len;
    MongoExtensionAPIVersion* versions;
} MongoExtensionAPIVersionVector;

/**
 * A read-only view of a byte array.
 */
typedef struct MongoExtensionByteView {
    const uint8_t* data;
    uint64_t len;
} MongoExtensionByteView;

/**
 * MongoExtensionByteBuf is an abstraction for an extension byte array buffer.
 * The underlying buffer is owned by MongoExtensionByteBuf.
 */
typedef struct MongoExtensionByteBuf {
    const struct MongoExtensionByteBufVTable* const vtable;
} MongoExtensionByteBuf;

/**
 * Virtual function table for MongoExtensionByteBuf.
 */
typedef struct MongoExtensionByteBufVTable {
    /**
     * Destroy `ptr` and free all associated resources.
     */
    void (*destroy)(MongoExtensionByteBuf* ptr);

    /**
     * Get a read-only view of the contents of `ptr`.
     */
    MongoExtensionByteView (*get_view)(const MongoExtensionByteBuf* ptr);
} MongoExtensionByteBufVTable;

/**
 * MongoExtensionStatus is an abstraction that allows a 'Status' to be passed across the extension
 * API boundary.
 *
 * This is typically returned by extensions to the host to communicate the result of an API call,
 * but may be provided to the extension by the host in some cases, like when a transform aggregation
 * stage consumes input from a host provided aggregation stage.
 */
typedef struct MongoExtensionStatus {
    const struct MongoExtensionStatusVTable* const vtable;
} MongoExtensionStatus;

/**
 * Virtual function table for MongoExtensionStatus.
 */
typedef struct MongoExtensionStatusVTable {
    /**
     * Destroy `status` and free all associated resources.
     */
    void (*destroy)(MongoExtensionStatus* status);

    /**
     * Return an error code associated with `status`.
     * A non-zero value indicates an error, otherwise, status is OK (success).
     */
    int32_t (*get_code)(const MongoExtensionStatus* status);

    /**
     * Return a utf-8 string associated with `status`. May be empty.
     */
    MongoExtensionByteView (*get_reason)(const MongoExtensionStatus* status);

    /**
     * Set an error code associated with `status`
     */
    void (*set_code)(MongoExtensionStatus* status, int32_t newCode);

    /**
     * Set a reason associated with `status`. May be empty.
     */
    MongoExtensionStatus* (*set_reason)(MongoExtensionStatus* status,
                                        MongoExtensionByteView newReason);

    /**
     * Clone this instance of MongoExtensionStatus.
     */
    MongoExtensionStatus* (*clone)(const MongoExtensionStatus* status,
                                   MongoExtensionStatus** output);
} MongoExtensionStatusVTable;

/**
 * MongoExtensionStatus error code reserved for errors which are not DBException errors generated
 * by calls back into the host, and which are not explicitly generated by the extension (i.e runtime
 * panics, exceptions, etc.)
 **/
const int32_t MONGO_EXTENSION_STATUS_RUNTIME_ERROR = -1;
const int32_t MONGO_EXTENSION_STATUS_OK = 0;

/**
 * Operation metrics exposed by extensions.
 *
 * This struct represents performance and execution statistics collected during extension
 * operations. Extensions can implement this interface to track and report various arbitrary metrics
 * about their execution, such as timing information, resource usage, or operation counts. The host
 * will periodically query these metrics for monitoring, diagnostics, and performance analysis
 * purposes.
 *
 * Extensions are responsible for implementing the collection and aggregation of metrics,
 * while the host is responsible for periodically retrieving, persisting, and exposing these metrics
 * through MongoDB's monitoring interfaces.
 *
 * Note that metrics are scoped to each operation - ie, query or getMore invocation. The lifetime of
 * the metrics is managed by the host and the extension should not persist or aggregate the metrics
 * itself across the query's lifetime.
 *
 * Metrics will be exposed via the serialize() function and prefaced by the extension's stage name.
 * For example, if an extension returned {counter: 1} from the serialize() implementation, the
 * metrics would be exposed via the host in the format {$stageName: {counter: 1}}.
 */
typedef struct MongoExtensionOperationMetrics {
    const struct MongoExtensionOperationMetricsVTable* vtable;
} MongoExtensionOperationMetrics;

typedef struct MongoExtensionOperationMetricsVTable {
    /**
     * Destroy `metrics` and free any related resources.
     */
    void (*destroy)(MongoExtensionOperationMetrics* metrics);

    /**
     * Serializes the collected metrics into an arbitrary BSON object. Ownership is allocated by the
     * extension and transferred to the host.
     */
    MongoExtensionStatus* (*serialize)(const MongoExtensionOperationMetrics* metrics,
                                       MongoExtensionByteBuf** output);

    /**
     * Updates and aggregates existing metrics with current execution metrics. Note that the
     * `arguments` byte view can be any format - for example, an opaque pointer, a serialized BSON
     * message, a serialized struct, etc.
     */
    MongoExtensionStatus* (*update)(MongoExtensionOperationMetrics* metrics,
                                    MongoExtensionByteView arguments);
} MongoExtensionOperationMetricsVTable;

/**
 * MongoExtensionQueryExecutionContext exposes helpers for an extension to call certain
 * functionality on a wrapped ExpressionContext. It is owned by the host and used by an extension.
 */
typedef struct MongoExtensionQueryExecutionContext {
    const struct MongoExtensionQueryExecutionContextVTable* vtable;
} MongoExtensionQueryExecutionContext;

// Forward declare
struct MongoExtensionExecAggStage;
typedef struct MongoExtensionQueryExecutionContextVTable {
    /**
     * Call checkForInterruptNoAssert() on the wrapped ExpressionContext and populate the
     * `queryStatus` with the resulting code/reason. Populates queryStatus with
     * MONGO_EXTENSION_STATUS_OK unless this operation is in a killed state. Note that the
     * MongoExtensionStatus `queryStatus` is owned by the extension and ownership is NOT
     * transferred to the host.
     */
    MongoExtensionStatus* (*check_for_interrupt)(const MongoExtensionQueryExecutionContext* ctx,
                                                 MongoExtensionStatus* queryStatus);

    /**
     * Check if any existing metrics for this extension exist on the wrapped OperationContext and
     * return an unowned pointer inside of `metrics`, to either a new set of metrics or the existing
     * set of metrics.
     *
     * When this method is first called during an operation (e.g. query or getMore), the host will
     * initialize a new set of metrics and return them. Otherwise, the existing metrics for the
     * current operation will be returned. Note that multiple instances of the same aggregation
     * stage in a single pipeline will share operation metrics.
     */
    MongoExtensionStatus* (*get_metrics)(const MongoExtensionQueryExecutionContext* ctx,
                                         MongoExtensionExecAggStage* execAggStage,
                                         MongoExtensionOperationMetrics** metrics);
} MongoExtensionQueryExecutionContextVTable;

/**
 * MongoExtensionHostQueryShapeOpts exposes helpers for an extension to serialize certain values
 * inside of a stage's BSON specification for query shape serialization.
 *
 * The pointer is only valid during a single call to serialization and should not be retained by the
 * extension.
 */
typedef struct MongoExtensionHostQueryShapeOpts {
    const struct MongoExtensionHostQueryShapeOptsVTable* vtable;
} MongoExtensionHostQueryShapeOpts;

typedef struct MongoExtensionHostQueryShapeOptsVTable {
    /**
     * Populates the ByteBuf with the serialized version of the identifier. Ownership is
     * transferred to the caller.
     */
    MongoExtensionStatus* (*serialize_identifier)(const MongoExtensionHostQueryShapeOpts* ctx,
                                                  MongoExtensionByteView ident,
                                                  MongoExtensionByteBuf** output);

    /**
     * Populates the ByteBuf with the serialized version of the field path. Ownership is
     * transferred to the caller.
     */
    MongoExtensionStatus* (*serialize_field_path)(const MongoExtensionHostQueryShapeOpts* ctx,
                                                  MongoExtensionByteView fieldPath,
                                                  MongoExtensionByteBuf** output);

    /**
     * Populates the ByteBuf with a serialized BSON object containing the serialized version of the
     * literal. Ownership is transferred to the caller.
     *
     * Note that this receives a BSONElement as input because literal serialization requires knowing
     * the type of the underlying value, and the value may be any valid BSONType - not necessarily
     * a string. The caller of `serialize_literal` must keep the buffer backing the BSONElement
     * alive across the call to `serialize_literal`.
     *
     * Returned BSON format: {"": <serializedLiteral>}
     */
    MongoExtensionStatus* (*serialize_literal)(const MongoExtensionHostQueryShapeOpts* ctx,
                                               MongoExtensionByteView bsonElement,
                                               MongoExtensionByteBuf** output);
} MongoExtensionHostQueryShapeOptsVTable;

/**
 * Possible explain verbosity levels.
 */
typedef enum MongoExtensionExplainVerbosity : uint32_t {
    /**
     * Display basic information about the pipeline that would run.
     */
    kQueryPlanner = 0,
    /**
     * In addition reporting basic information about the pipeline, runs the pipeline and reports
     * execution-related stats.
     */
    kExecStats = 1,
    /**
     * Generates kExecStats output for all possible query plans.
     */
    kExecAllPlans = 2,
} MongoExtensionExplainVerbosity;

/**
 * An AggStageDescriptor describes features of a stage that are not bound to the stage
 * definition. This object functions as a factory to create logical stage through parsing.
 *
 * These objects are owned by extensions so no method is provided to free them.
 */
typedef struct MongoExtensionAggStageDescriptor {
    const struct MongoExtensionAggStageDescriptorVTable* const vtable;
} MongoExtensionAggStageDescriptor;

/**
 * Virtual function table for MongoExtensionAggStageDescriptor.
 */
typedef struct MongoExtensionAggStageDescriptorVTable {
    /**
     * Returns a MongoExtensionByteView containing the name of this aggregation stage.
     */
    MongoExtensionByteView (*get_name)(const MongoExtensionAggStageDescriptor* descriptor);

    /**
     * Parse the user provided stage definition into a parse node.
     *
     * stageBson contains a BSON document with a single (stageName, stageDefinition) element
     * tuple. In case of success, `*parseNode` is populated with the parsed stage. Both the
     * returned MongoExtensionStatus and `parseNode` are owned by the caller.
     */
    MongoExtensionStatus* (*parse)(const MongoExtensionAggStageDescriptor* descriptor,
                                   MongoExtensionByteView stageBson,
                                   struct MongoExtensionAggStageParseNode** parseNode);
} MongoExtensionAggStageDescriptorVTable;

/**
 * A MongoExtensionLogicalAggStage describes a stage that has been parsed and bound to
 * instance specific context -- the stage definition and other context data from the pipeline.
 * These objects are suitable for pipeline optimization. Once optimization is complete they can
 * be used to generate objects for execution.
 */
typedef struct MongoExtensionLogicalAggStage {
    const struct MongoExtensionLogicalAggStageVTable* const vtable;
} MongoExtensionLogicalAggStage;

/**
 * Virtual function table for MongoExtensionLogicalAggStage.
 */
typedef struct MongoExtensionLogicalAggStageVTable {
    /**
     * Destroy `logicalStage` and free any related resources.
     */
    void (*destroy)(MongoExtensionLogicalAggStage* logicalStage);

    /**
     * Returns a MongoExtensionByteView containing the name of the associated aggregation stage.
     */
    MongoExtensionByteView (*get_name)(const MongoExtensionLogicalAggStage* logicalStage);

    /**
     * Serialize `logicalStage` to be potentially sent across the wire to other execution nodes.
     */
    MongoExtensionStatus* (*serialize)(const MongoExtensionLogicalAggStage* logicalStage,
                                       MongoExtensionByteBuf** output);

    /**
     * Populates the ByteBuf with the stage's explain output as serialized BSON. Ownership is
     * transferred to the caller.
     *
     * Output is expected to be in the form {$stageName: {...}}.
     *
     * Note that this method will be called for all three verbosity levels, but will only populate
     * the query plan portion of explain.
     */
    MongoExtensionStatus* (*explain)(const MongoExtensionLogicalAggStage* logicalStage,
                                     MongoExtensionExplainVerbosity verbosity,
                                     MongoExtensionByteBuf** output);

    /**
     * compile: On success, "compiles" the LogicalStage into an ExecutableStage, populating the
     * output parameter ExecutableStage pointer with the extension's executable stage. Ownership is
     * transferred to the caller.
     */
    MongoExtensionStatus* (*compile)(const MongoExtensionLogicalAggStage* logicalStage,
                                     struct MongoExtensionExecAggStage** output);

    /**
     * Populates the output with an extension stage's DistributedPlanLogic, which specifies how
     * results from shards should be merged in a sharded cluster. If a stage can run fully in
     * parallel on the shards, the output pointer is not populated and is left as a nullptr.
     *
     * Ownership of the MongoExtensionDistributedPlanLogic is transferred to the caller.
     */
    MongoExtensionStatus* (*get_distributed_plan_logic)(
        const MongoExtensionLogicalAggStage* logicalStage,
        struct MongoExtensionDistributedPlanLogic** output);

} MongoExtensionLogicalAggStageVTable;

/**
 * Types of nodes that can be in an ExpandedArray.
 */
typedef enum MongoExtensionAggStageNodeType : uint32_t {
    kParseNode = 0,
    kAstNode = 1
} MongoExtensionAggStageNodeType;

/**
 * MongoExtensionAggStageParseNode is responsible for validating the user provided syntax,
 * generating a query shape, and expanding into a resolved list of nodes that can be either AST
 * nodes or other parse nodes (which will eventually resolve to AST nodes through recursive
 * expansion). It can participate in preliminary pipeline validation.
 */
typedef struct MongoExtensionAggStageParseNode {
    const struct MongoExtensionAggStageParseNodeVTable* const vtable;
} MongoExtensionAggStageParseNode;

/**
 * An AggStageAstNode describes an aggregation stage that has been parsed and expanded into
 * a form that can participate in lite-parsed validation.
 */
typedef struct MongoExtensionAggStageAstNode {
    const struct MongoExtensionAggStageAstNodeVTable* const vtable;
} MongoExtensionAggStageAstNode;

typedef struct MongoExtensionExpandedArrayElement {
    MongoExtensionAggStageNodeType type;
    union {
        MongoExtensionAggStageParseNode* parse;
        MongoExtensionAggStageAstNode* ast;
    } parseOrAst;
} MongoExtensionExpandedArrayElement;

/**
 * MongoExtensionExpandedArray is an abstraction to represent the expansion of a ParseNode. A
 * ParseNode can expand into a MongoExtensionExpandedArrayElement array of one or more elements.
 * Each element is either a ParseNode or an AstNode. The array is allocated and owned by the caller
 * and populated by the extension.
 */
typedef struct MongoExtensionExpandedArray {
    size_t size;
    struct MongoExtensionExpandedArrayElement* const elements;
} MongoExtensionExpandedArray;

/**
 * Types of elements that can be in a MongoExtensionDPLArray.
 */
typedef enum MongoExtensionDPLArrayElementType : uint32_t {
    kParse = 0,   // Parse node
    kLogical = 1  // Logical stage
} MongoExtensionDPLArrayElementType;

/**
 * MongoExtensionDPLArrayElement represents a single element in a MongoExtensionDPLArray. Each
 * element can be either a parse node or a logical stage.
 *
 * If an element is a logical stage, it must be the same stage type as the logical stage that
 * generated it.
 */
typedef struct MongoExtensionDPLArrayElement {
    // Indicates what type the element is.
    MongoExtensionDPLArrayElementType type;
    union {
        MongoExtensionAggStageParseNode* parseNode;
        MongoExtensionLogicalAggStage* logicalStage;
    } element;
} MongoExtensionDPLArrayElement;

/**
 * MongoExtensionDPLArray represents an array of elements used during distributed planning. The
 * array can contain either parse nodes or logical stages.
 *
 * Once the MongoExtensionDPLArray is populated by the extension, ownership is assumed to be
 * transferred entirely to the Host.
 */
typedef struct MongoExtensionDPLArray {
    size_t size;
    struct MongoExtensionDPLArrayElement* const elements;
} MongoExtensionDPLArray;

/**
 * MongoExtensionDPLArrayContainer wraps an extension-implemented array that must be transferred
 * into a Host pre-allocated array.
 *
 * This container allows extensions to provide arrays of stages (either parse nodes or logical
 * stages) for distributed planning without going through a serialize/parse cycle. The Host
 * pre-allocates the target array and the extension transfers ownership of the elements into it.
 */
typedef struct MongoExtensionDPLArrayContainer {
    const struct MongoExtensionDPLArrayContainerVTable* const vtable;
} MongoExtensionDPLArrayContainer;

/**
 * Virtual function table for MongoExtensionDPLArrayContainer.
 */
typedef struct MongoExtensionDPLArrayContainerVTable {
    /**
     * Destroy `container` and free all associated resources.
     */
    void (*destroy)(MongoExtensionDPLArrayContainer* container);

    /**
     * Returns the number of elements in the DPLArrayContainer.
     * Callers must first obtain the size before calling transfer() in order to
     * pre-allocate the target output array.
     */
    size_t (*size)(const MongoExtensionDPLArrayContainer* container);

    /**
     * Transfers ownership of the underlying DPLArrayContainer's elements into
     * the target array.
     * Callers must first obtain the size of the array in order to pre-allocate the
     * target output array.
     * Ownership of the pointers within the array elements is transferred to the caller.
     * It is an error to provide an incorrectly sized output array.
     */
    MongoExtensionStatus* (*transfer)(MongoExtensionDPLArrayContainer* container,
                                      MongoExtensionDPLArray* array);
} MongoExtensionDPLArrayContainerVTable;

/**
 * MongoExtensionDistributedPlanLogic is an abstraction representing the information needed to
 * execute this stage on a distributed collection. It describes how a pipeline should be split for
 * sharded execution.
 */
typedef struct MongoExtensionDistributedPlanLogic {
    const struct MongoExtensionDistributedPlanLogicVTable* const vtable;
} MongoExtensionDistributedPlanLogic;

typedef struct MongoExtensionDistributedPlanLogicVTable {
    /**
     * Destroys `distributedPlanLogic` and frees any related resources.
     */
    void (*destroy)(MongoExtensionDistributedPlanLogic* distributedPlanLogic);

    /**
     * Returns the pipeline to execute on each shard in parallel.
     * On success, if the stage has a component that can run on the shards, allocates a
     * MongoExtensionDPLArrayContainer with the stages that make up the shards pipeline. The
     * extension populates the provided output pointer, transferring ownership of the container to
     * the caller. If a stage must run exclusively on the merging node, the output pointer is
     * returned as a nullptr.
     *
     * This method may only be called once.
     *
     * Note: This is currently restricted to only a single shardsStage for parity with the
     * DistributedPlanLogic shardsStage. This single shardsStage must be fully expanded (i.e. not a
     * desugar stage) so that it can be converted to a single DocumentSource. If in the future an
     * extension stage may return more than one shardsStage, we will remove that restriction and
     * modify DistributedPlanLogic.
     */
    MongoExtensionStatus* (*extract_shards_pipeline)(
        MongoExtensionDistributedPlanLogic* distributedPlanLogic,
        MongoExtensionDPLArrayContainer** output);

    /**
     * Returns the stages that will be run on the merging node.
     * On success, if the stage has a component that must run on the merging node, allocates a
     * MongoExtensionDPLArrayContainer with the stages that make up the merge pipeline. The
     * extension populates the provided output pointer, transferring ownership of the container to
     * the caller. If nothing can run on the merging node, the output pointer is returned as a
     * nullptr.
     *
     * This method may only be called once.
     */
    MongoExtensionStatus* (*extract_merging_pipeline)(
        MongoExtensionDistributedPlanLogic* distributedPlanLogic,
        MongoExtensionDPLArrayContainer** output);

    /**
     * Returns which fields are ascending and which fields are descending when merging streams
     * together. Ownership of the ByteBuf is transferred to the caller. The MongoExtensionByteBuf
     * will not be allocated if no sort pattern is required to merge the streams.
     *
     * Note: Specifying a sort pattern via DistributedPlanLogic will not be enough to execute
     * the distributed sort. get_next() on the MongoExtensionExecAggStage must also set the
     * $sortKey metadata field on each output document. Returning a non-empty sort pattern here but
     * not setting the sort key metadata on output documents will result in a runtime error.
     */
    MongoExtensionStatus* (*get_sort_pattern)(
        MongoExtensionDistributedPlanLogic* distributedPlanLogic, MongoExtensionByteBuf** output);
} MongoExtensionDistributedPlanLogicVTable;

/**
 * Virtual function table for MongoExtensionAggStageParseNode.
 */
typedef struct MongoExtensionAggStageParseNodeVTable {
    /**
     * Destroys object and frees related resources.
     */
    void (*destroy)(MongoExtensionAggStageParseNode* parseNode);

    /**
     * Returns a MongoExtensionByteView containing the name of the associated aggregation stage.
     */
    MongoExtensionByteView (*get_name)(const MongoExtensionAggStageParseNode* parseNode);

    /**
     * Populates the ByteBuf with the stage's query shape as serialized BSON. Ownership is
     * transferred to the caller.
     */
    MongoExtensionStatus* (*get_query_shape)(const MongoExtensionAggStageParseNode* parseNode,
                                             const MongoExtensionHostQueryShapeOpts* ctx,
                                             MongoExtensionByteBuf** queryShape);

    /**
     * Returns the size (number of nodes) of the ParseNode's expansion. Callers must first get the
     * expansion size before calling expand() so that they can pre-allocate the ExpandedArrayElement
     * array which is then populated by the extension.
     */
    size_t (*get_expanded_size)(const MongoExtensionAggStageParseNode* parseNode);

    /**
     * Populates the MongoExtensionExpandedArray with the stage's expansion.
     *
     * If a stage does not desugar, the ExpandedArray will contain a single AstNode representation
     * for this ParseNode. If a stage desugars into multiple stages, the ExpandedArray will contain
     * the component stages' representation.
     *
     * The expanded array must contain at least one element.
     *
     * The caller must first get the expansion size and allocate the ExpandedArray
     * elements buffer with the correct size before calling expand() with the allocated buffer which
     * will be populated with the elements.
     *
     * The caller is responsible for fully expanding the returned expanded array recursively.
     *
     * Ownership of the pointers within the array elements is transferred to the caller.
     */
    MongoExtensionStatus* (*expand)(const MongoExtensionAggStageParseNode* parseNode,
                                    MongoExtensionExpandedArray* expanded);
} MongoExtensionAggStageParseNodeVTable;

/**
 * Virtual function table for MongoExtensionAggStageAstNode.
 */
typedef struct MongoExtensionAggStageAstNodeVTable {
    /**
     * Destroys `astNode` and free any related resources.
     */
    void (*destroy)(MongoExtensionAggStageAstNode* astNode);

    /**
     * Returns a MongoExtensionByteView containing the name of the associated aggregation stage.
     */
    MongoExtensionByteView (*get_name)(const MongoExtensionAggStageAstNode* astNode);

    /**
     * Returns static properties of this stage related to pipeline optimization as a serialized BSON
     * document.
     */
    MongoExtensionStatus* (*get_properties)(const MongoExtensionAggStageAstNode* astNode,
                                            MongoExtensionByteBuf** properties);

    /**
     * Populates `logicalStage` with the stage's runtime implementation of the optimization
     * interface, ownership of which is transferred to the caller. This step should be called after
     * validating `astNode` and is used when converting into an optimizable stage.
     */
    MongoExtensionStatus* (*bind)(const MongoExtensionAggStageAstNode* astNode,
                                  MongoExtensionLogicalAggStage** logicalStage);
} MongoExtensionAggStageAstNodeVTable;

/**
 * Code indicating the result of a getNext() call.
 */
typedef enum MongoExtensionGetNextResultCode : uint8_t {
    /**
     * getNext() yielded a document.
     */
    kAdvanced = 0,

    /**
     * getNext() and the document stream was exhausted. Subsequent calls will not yield any more
     * documents.
     */
    kEOF = 1,

    /**
     * getNext() did not yield a document, but may yield another document in the future.
     */
    kPauseExecution = 2,
} MongoExtensionGetNextResultCode;

typedef enum MongoExtensionByteContainerType : uint8_t {
    kByteView = 0,
    kByteBuf = 1,
} MongoExtensionByteContainerType;

/**
 * MongoExtensionByteContainer is an abstraction to represent a serialized ByteBuf or ByteView.
 * Depending on the type enum specified, this struct will contain either a ByteBuf with ownership
 * being transferred to the caller, or a ByteView which the callee guarantees to remain valid for a
 * specified duration.
 */
typedef struct MongoExtensionByteContainer {
    MongoExtensionByteContainerType type;
    union {
        MongoExtensionByteView view;
        MongoExtensionByteBuf* buf;
    } bytes;
} MongoExtensionByteContainer;

/**
 * MongoExtensionGetNextResult is a container used to fetch results (with or without metadata) from
 * an ExecutableStage's get_next() function. Callers of ExecutableStage::get_next() are responsible
 * for instantiating this struct and passing the corresponding pointer to the function invocation.
 */
typedef struct MongoExtensionGetNextResult {
    MongoExtensionGetNextResultCode code;
    MongoExtensionByteContainer resultDocument;
    MongoExtensionByteContainer resultMetadata;
} MongoExtensionGetNextResult;

/**
 * MongoExtensionExecAggStage is the abstraction representing the executable phase of
 * a stage by the extension.
 */
typedef struct MongoExtensionExecAggStage {
    const struct MongoExtensionExecAggStageVTable* const vtable;
} MongoExtensionExecAggStage;

/**
 * Virtual function table for MongoExtensionExecAggStage.
 */
typedef struct MongoExtensionExecAggStageVTable {
    /**
     * Destroys object and frees related resources.
     */
    void (*destroy)(MongoExtensionExecAggStage* execAggStage);

    /**
     * Pulls the next result from the stage executor.
     *
     * On success:
     *  - Updates the provided MongoExtensionGetNextResult with a result code
     *    indicating whether or not a document has been returned.
     *  - If a document is available, return the MongoExtensionByteContainer document as one of the
     * following:
     *       * a MongoExtensionByteBuf (kByteBuf)
     *       * a MongoExtensionByteView (kByteView)
     *
     * Ownership / lifetime:
     *  - For kByteBuf: ownership of the buffer is transferred to the caller (ex: host).
     *  - For kByteView: the callee (ex: extension) retains ownership of the underlying memory
     *    and MUST keep it valid and unchanged until the next call to get_next()
     *    on this execAggStage or until destroy() is called. The caller must treat
     *    the view as read-only and must not free it.
     */
    MongoExtensionStatus* (*get_next)(MongoExtensionExecAggStage* execAggStage,
                                      MongoExtensionQueryExecutionContext* execCtxPtr,
                                      MongoExtensionGetNextResult* getNextResult);

    /**
     * Returns a MongoExtensionByteView containing the name of the associated aggregation stage.
     */
    MongoExtensionByteView (*get_name)(const MongoExtensionExecAggStage* astNode);

    /**
     * Creates a MongoExtensionOperationMetrics object to collect metrics for this aggregation
     * stage, then populates `metrics` with the location. Ownership of the metrics object is
     * transferred to the caller.
     */
    MongoExtensionStatus* (*create_metrics)(const MongoExtensionExecAggStage* execAggStage,
                                            MongoExtensionOperationMetrics** metrics);

    /**
     * Sets the source input stage for the extension stage. Ownership is NOT transferred to the
     * caller.
     */
    MongoExtensionStatus* (*set_source)(MongoExtensionExecAggStage* execAggStage,
                                        MongoExtensionExecAggStage* sourceStage);
    /**
     * Initializes the stage and positions it before the first result.
     * Resources should be acquired during open() and avoided in getNext() for better
     * performance.
     */
    MongoExtensionStatus* (*open)(MongoExtensionExecAggStage* execAggStage);

    /**
     * Reinitializes acquired resources. Semantically equivalent to close() + open(), but more
     * efficient.
     */
    MongoExtensionStatus* (*reopen)(MongoExtensionExecAggStage* execAggStage);

    /**
     * Frees all acquired resources.
     */
    MongoExtensionStatus* (*close)(MongoExtensionExecAggStage* execAggStage);

    /**
     * Populates the ByteBuf with the stage's explain output as serialized BSON. Ownership is
     * transferred to the caller.
     *
     * Output is expected to be in the form {metricA: val1, metricB: val2, ...}}.
     *
     * Note that this method will be called for verbosity levels >= 'executionStats', and will only
     * populate the execution metrics portion of the explain output.
     */
    MongoExtensionStatus* (*explain)(const MongoExtensionExecAggStage* execAggStage,
                                     MongoExtensionExplainVerbosity verbosity,
                                     MongoExtensionByteBuf** output);
} MongoExtensionExecAggStageVTable;

/**
 * MongoExtensionHostPortal serves as the entry point for extensions to integrate with the
 * server. It exposes a function pointer, registerStageDescriptor, which allows extensions to
 * register custom aggregation stages.
 */
typedef struct MongoExtensionHostPortal {
    const struct MongoExtensionHostPortalVTable* vtable;
    /**
     * The version of the Extensions API that the host and extension agreed upon when creating
     * the MongoExtension.
     */
    MongoExtensionAPIVersion hostExtensionsAPIVersion;

    /**
     * Wire versions in MongoDB are stored in an enum. Each service context will have both a min and
     * a max wire version; the extension should only need the max wire version in order to determine
     * if new server features have been added.
     */
    int32_t hostMongoDBMaxWireVersion;
} MongoExtensionHostPortal;

/**
 * Virtual function table for MongoExtensionHostPortal.
 */
typedef struct MongoExtensionHostPortalVTable {
    /**
     * Register an aggregation stage descriptor with the host.
     */
    MongoExtensionStatus* (*register_stage_descriptor)(
        const MongoExtensionHostPortal* hostPortal,
        const MongoExtensionAggStageDescriptor* descriptor);

    /**
     * Returns a MongoExtensionByteView containing the raw extension options associated with this
     * extension.
     */
    MongoExtensionByteView (*get_extension_options)(const MongoExtensionHostPortal* portal);
} MongoExtensionHostPortalVTable;

/**
 * Represents a single key-value pair attribute for a structured log message. Both `name` and
 * `value` are expected to be strings serialized to ByteViews.
 *
 * These attributes provide additional context and metadata for extension log messages,
 * allowing structured logging with arbitrary metadata beyond the base message text.
 */
typedef struct MongoExtensionLogAttribute {
    MongoExtensionByteView name;
    MongoExtensionByteView value;
} MongoExtensionLogAttribute;

/**
 * A fixed-size array of log attributes that accompany a structured log message.
 *
 * The array is allocated by the caller and populated with attributes to be logged
 * alongside a structured log message. The `elements` pointer references an array of
 * `size` MongoExtensionLogAttribute entries.
 */
typedef struct MongoExtensionLogAttributesArray {
    uint64_t size;
    struct MongoExtensionLogAttribute* elements;
} MongoExtensionLogAttributesArray;

/**
 * Log severity levels for extension log messages.
 */
typedef enum MongoExtensionLogSeverity : uint32_t {
    kError,
    kWarning,
    kInfo
} MongoExtensionLogSeverity;

/**
 * Types of log messages. kLog type will always be logged, and kDebug type will be logged if the
 * server's current log level is >= the specified debug log level.
 */
typedef enum MongoExtensionLogType : uint32_t { kLog, kDebug } MongoExtensionLogType;

/**
 * A structured log message from an extension.
 */
typedef struct MongoExtensionLogMessage {
    uint32_t code;
    MongoExtensionByteView message;
    MongoExtensionLogType type;
    MongoExtensionLogAttributesArray attributes;
    union {
        MongoExtensionLogSeverity severity;
        int level;
    } severityOrLevel;
} MongoExtensionLogMessage;

/**
 * MongoExtensionIdleThreadBlock enables extension-spawned threads to be marked as idle, which means
 * they will be excluded from multi-threaded gdb stacktraces.
 *
 * Only the 'destroy' function is needed as the idle functionality will be handled by a
 * host-constructed adapter, so the API struct is only responsible for providing a bridge
 * to transfer ownership from said adapter to the extension-side handle.
 */
typedef struct MongoExtensionIdleThreadBlock {
    const struct MongoExtensionIdleThreadBlockVTable* const vtable;
} MongoExtensionIdleThreadBlock;

typedef struct MongoExtensionIdleThreadBlockVTable {
    void (*destroy)(MongoExtensionIdleThreadBlock*);
} MongoExtensionIdleThreadBlockVTable;

/**
 * MongoExtensionLogger enables extensions to send structured log messages to MongoDB's logging
 * system.
 *
 * The logger is implemented by the host and provided to extensions through `HostServices`.
 *
 * The logger supports multiple severity levels (Info, Warning, Error) for standard logs and
 * debug levels (1-5) for debug logs, allowing extensions to categorize messages by importance
 * and emit debug traces conditionally based on server log level configuration.
 */
typedef struct MongoExtensionLogger {
    const struct MongoExtensionLoggerVTable* vtable;
} MongoExtensionLogger;

/**
 * Virtual function table for MongoExtensionLogger.
 */
typedef struct MongoExtensionLoggerVTable {
    /**
     * Logs a message from the extension. The log may be a severity log with severity INFO, WARNING,
     * or ERROR. It may also be a debug log w/ a numeric debug log level.
     */
    MongoExtensionStatus* (*log)(const MongoExtensionLogMessage* rawLog);

    /**
     * This provides an optimization to the logging service, as it compares the provided log
     * level/severity against the server's current log level before materializing and sending a log
     * over the wire. 'logType' indicates whether levelOrSeverity is a level (kDebug) or a severity
     * (kLog), as in the latter case in case we need to transform the value to a logv2::LogSeverity.
     */
    MongoExtensionStatus* (*should_log)(MongoExtensionLogSeverity levelOrSeverity,
                                        ::MongoExtensionLogType logType,
                                        bool* out);
} MongoExtensionLoggerVTable;

/**
 * MongoExtensionHostServices exposes services provided by the host to the extension.
 *
 * Currently, the VTable struct is a placeholder for future services.
 */
typedef struct MongoExtensionHostServices {
    const struct MongoExtensionHostServicesVTable* vtable;
} MongoExtensionHostServices;

/**
 * Virtual function table for MongoExtensionHostServices.
 */
typedef struct MongoExtensionHostServicesVTable {
    /**
     * Retrieve the static logging instance on the host.
     */
    MongoExtensionLogger* (*get_logger)();

    /**
     * Throws a non-fatal exception to end the current operation with an error. This should be
     * called when the user made an error.
     */
    MongoExtensionStatus* (*user_asserted)(MongoExtensionByteView structuredErrorMessage);

    /**
     * Like user_asserted, but with a deferred-fatality tripwire that gets checked prior to normal
     * shutdown. Used to ensure that this assertion will both fail the operation and also cause a
     * test suite failure.
     */
    MongoExtensionStatus* (*tripwire_asserted)(MongoExtensionByteView structuredErrorMessage);

    /**
     * Call this method to mark an extension-owned thread as idle. This will cause the thread to be
     * omitted from gdb stacktraces when using the 'mongodb-bt-if-active' command. The thread will
     * remain idle as long as the owned handle for 'idleThreadBlock' remains in scope.
     *
     * Location must be a null-terminated c string, so either a string literal or a stable char*.
     * For ease of use, the MONGO_EXTENSION_IDLE_LOCATION macro will pass in the location in the
     * correct format.
     */
    MongoExtensionStatus* (*mark_idle_thread_block)(MongoExtensionIdleThreadBlock** idleThreadBlock,
                                                    const char* location);
    /*
     * Creates a host-defined parse node. Use this function when you need to instantiate a parse
     * node implemented by the host during extension parse node expansion.
     *
     * 'bsonSpec' is a view on the BSON specification of the host aggregation stage and is owned by
     * the caller. The out-parameter 'node' pointer remains owned by the host.
     */
    MongoExtensionStatus* (*create_host_agg_stage_parse_node)(
        MongoExtensionByteView bsonSpec, MongoExtensionAggStageParseNode** node);

    /**
     * Creates a host-defined AST node for an $_internalSearchIdLookup stage. If the provided
     * bsonSpec does not specify a valid $_internalSearchIdLookup stage, an error is returned. On
     * success, 'node' is populated with the host's AST node.
     */
    MongoExtensionStatus* (*create_id_lookup)(MongoExtensionByteView bsonSpec,
                                              MongoExtensionAggStageAstNode** node);
} MongoExtensionHostServicesVTable;

/**
 * MongoExtension is the top-level struct that must be defined by any MongoDB extension. It contains
 * the API version and an initialization function.
 *
 * At extension loading time, the MongoDB server will check compatibility of the extension's API
 * version with the server's API version then invoke the initializer. We also provide a pointer to
 * the host services for the extension to invoke provided host functionality at any point.
 *
 * The host _portal_ pointer is only valid during initialization and should not be retained by the
 * extension. The host _services_ pointer, on the other hand, is valid for the lifetime of the
 * extension and should be saved by the extension.
 */
typedef struct MongoExtension {
    const struct MongoExtensionVTable* const vtable;
    MongoExtensionAPIVersion version;
} MongoExtension;

/**
 * Virtual function table for MongoExtension.
 */
typedef struct MongoExtensionVTable {
    /**
     * Initialize the extension, passing in a pointer to the host portal.
     *
     * The host portal pointer is only valid during initialization and should not be retained by the
     * extension to avoid a dangling pointer.
     */
    MongoExtensionStatus* (*initialize)(const MongoExtension* extension,
                                        const MongoExtensionHostPortal* portal,
                                        const MongoExtensionHostServices* services);
} MongoExtensionVTable;

/**
 * The symbol that must be defined in all extension shared libraries to register the extension with
 * the MongoDB server when the extension is loaded. Returns a MongoExtensionStatus indicating
 * whether or not the parameter MongoExtension was successfully initialized. Also takes a struct
 * representing the API version requirements to comply with the host.
 *
 * NOTE: You must define this symbol in your extension shared library and avoid name mangling (for
 * example, with 'extern "C"') so that the MongoDB server can find it at loadtime.
 */
#define GET_MONGODB_EXTENSION_SYMBOL "get_mongodb_extension"
typedef MongoExtensionStatus* (*get_mongo_extension_t)(
    const MongoExtensionAPIVersionVector* hostVersions, const MongoExtension** extension);

#ifdef __cplusplus
}  // extern "C"
#endif
