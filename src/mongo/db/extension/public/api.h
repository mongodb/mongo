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

#include "mongo/util/modules.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Represents the API version of the MongoDB extension, to ensure compatibility between the MongoDB
 * server and the extension.
 *
 * The version is composed of three parts: major, minor, and patch. The major version is incremented
 * for incompatible changes, the minor version for for backward-compatible changes, and the patch
 * version for bug fixes.
 */
typedef struct {
    uint32_t major;
    uint32_t minor;
    uint32_t patch;
} MongoExtensionAPIVersion;

#define MONGODB_EXTENSION_API_MAJOR_VERSION 0
#define MONGODB_EXTENSION_API_MINOR_VERSION 0
#define MONGODB_EXTENSION_API_PATCH_VERSION 0

// The current API version of the MongoDB extension.
#define MONGODB_EXTENSION_API_VERSION                                             \
    MongoExtensionAPIVersion {                                                    \
        MONGODB_EXTENSION_API_MAJOR_VERSION, MONGODB_EXTENSION_API_MINOR_VERSION, \
            MONGODB_EXTENSION_API_PATCH_VERSION                                   \
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
    void (*set_reason)(MongoExtensionStatus* status, MongoExtensionByteView newReason);
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
                                         const MongoExtensionExecAggStage* execAggStage,
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
     * Note that this method will be called for all three verbosity levels.
     */
    MongoExtensionStatus* (*explain)(const MongoExtensionLogicalAggStage* logicalStage,
                                     MongoExtensionExplainVerbosity verbosity,
                                     MongoExtensionByteBuf** output);
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
    };
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

/**
 * MongoExtensionGetNextResult is a container used to fetch results from an
 * ExecutableStage's get_next() function. Callers of ExecutableStage::get_next() are responsible for
 * instantiating this struct and passing the corresponding pointer to the function invocation.
 */
typedef struct MongoExtensionGetNextResult {
    MongoExtensionGetNextResultCode code;
    MongoExtensionByteBuf* result;
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
     * On success:
     *    - Updates the provided MongoExtensionGetNextResult with a result code
     *      indicating whether or not a document has been returned by the function.
     *    - If the result code indicates a document is available, populates
     *      MongoExtensionGetNextResult's ByteBuf pointer with the resulting document as
     *      a byte buffer. Ownership of the buffer is transferred to the Host.
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
    // TODO SERVER-111339 Add attributes.
    union {
        MongoExtensionLogSeverity severity;
        int level;
    } severityOrLevel;
} MongoExtensionLogMessage;

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
     * Logs a message from the extension with severity INFO, WARNING, or ERROR.
     */
    MongoExtensionStatus* (*log)(const MongoExtensionLogMessage* rawLog);

    /**
     * Sends a debug log message to the server, and logs it as long as the 'Extension' log component
     * in the server has a level greater or equal to the debug log's level.
     */
    MongoExtensionStatus* (*log_debug)(const MongoExtensionLogMessage* rawLog);

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

    /**
     * This provides an optimization to the logging service, as it compares the provided log
     * level/severity against the server's current log level before materializing and sending a log
     * over the wire. 'logType' indicates whether levelOrSeverity is a level (kDebug) or a severity
     * (kLog), as in the latter case in case we need to transform the value to a logv2::LogSeverity.
     */
    MongoExtensionStatus* (*should_log)(MongoExtensionLogSeverity levelOrSeverity,
                                        ::MongoExtensionLogType logType,
                                        bool* out);
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
