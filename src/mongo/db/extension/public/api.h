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
} MongoExtensionStatusVTable;

/**
 * MongoExtensionStatus error code reserved for errors which are not DBException errors generated
 * by calls back into the host, and which are not explicitly generated by the extension (i.e runtime
 * panics, exceptions, etc.)
 **/
const int32_t MONGO_EXTENSION_STATUS_RUNTIME_ERROR = -1;
const int32_t MONGO_EXTENSION_STATUS_OK = 0;

/**
 * Types of aggregation stages that can be implemented as an extension.
 */
typedef enum MongoExtensionAggregationStageType : uint32_t {
    /**
     * NoOp stage.
     */
    kNoOp = 0,
    kDesugar = 1,
} MongoExtensionAggregationStageType;

/**
 * An AggregationStageDescriptor describes features of a stage that are not bound to the stage
 * definition. This object functions as a factory to create logical stage through parsing.
 *
 * These objects are owned by extensions so no method is provided to free them.
 */
typedef struct MongoExtensionAggregationStageDescriptor {
    const struct MongoExtensionAggregationStageDescriptorVTable* const vtable;
} MongoExtensionAggregationStageDescriptor;

/**
 * Virtual function table for MongoExtensionAggregationStageDescriptor.
 */
typedef struct MongoExtensionAggregationStageDescriptorVTable {
    /**
     * Return the type for this stage.
     */
    MongoExtensionAggregationStageType (*get_type)(
        const MongoExtensionAggregationStageDescriptor* descriptor);

    /**
     * Returns a MongoExtensionByteView containing the name of this aggregation stage.
     */
    MongoExtensionByteView (*get_name)(const MongoExtensionAggregationStageDescriptor* descriptor);

    /**
     * Parse the user provided stage definition into a logical stage.
     *
     * stageBson contains a BSON document with a single (stageName, stageDefinition) element
     * tuple. In case of success, *logicalStage is populated with the parsed stage. Both the
     * returned MongoExtensionStatus and logicalStage is owned by the caller.
     */
    MongoExtensionStatus* (*parse)(const MongoExtensionAggregationStageDescriptor* descriptor,
                                   MongoExtensionByteView stageBson,
                                   struct MongoExtensionLogicalAggregationStage** logicalStage);

    /**
     * Populates MongoExtensionByteBuf pointer with the stage's expanded pipeline as serialized BSON
     * if it desugars. If the stage doesn't desugar, the pointer is not populated.
     *
     * Both the returned MongoExtensionStatus and expandedPipelineBSON are owned by the caller.
     */
    MongoExtensionStatus* (*expand)(const MongoExtensionAggregationStageDescriptor* descriptor,
                                    MongoExtensionByteBuf** expandedPipelineBSON);
} MongoExtensionAggregationStageDescriptorVTable;

/**
 * A MongoExtensionLogicalAggregationStage describes a stage that has been parsed and bound to
 * instance specific context -- the stage definition and other context data from the pipeline.
 * These objects are suitable for pipeline optimization. Once optimization is complete they can
 * be used to generate objects for execution.
 */
typedef struct MongoExtensionLogicalAggregationStage {
    const struct MongoExtensionLogicalAggregationStageVTable* const vtable;
} MongoExtensionLogicalAggregationStage;

/**
 * Virtual function table for MongoExtensionLogicalAggregationStage.
 */
typedef struct MongoExtensionLogicalAggregationStageVTable {
    /**
     * Destroy `logicalStage` and free any related resources.
     */
    void (*destroy)(MongoExtensionLogicalAggregationStage* logicalStage);
} MongoExtensionLogicalAggregationStageVTable;

/**
 * MongoExtensionHostPortal serves as the entry point for extensions to integrate with the
 * server. It exposes a function pointer, registerStageDescriptor, which allows extensions to
 * register custom aggregation stages.
 */
typedef struct MongoExtensionHostPortal {
    const struct MongoExtensionHostPortalVTable* vtable;
    MongoExtensionAPIVersion hostExtensionsAPIVersion;
    // Wire versions in MongoDB are stored in an enum. Each service context will have both a min and
    // a max wire version; the extension should only need the max wire version in order to determine
    // if new server features have been added.
    int32_t hostMongoDBMaxWireVersion;
} MongoExtensionHostPortal;

typedef struct MongoExtensionHostPortalVTable {
    MongoExtensionStatus* (*registerStageDescriptor)(
        const MongoExtensionAggregationStageDescriptor* descriptor);
} MongoExtensionHostPortalVTable;

/**
 * MongoExtension is the top-level struct that must be defined by any MongoDB extension. It contains
 * the API version and an initialization function.
 *
 * At extension loading time, the MongoDB server will check compatibility of the extension's API
 * version with the server's API version then invoke the initializer.
 */
typedef struct MongoExtension {
    const struct MongoExtensionVTable* const vtable;
    MongoExtensionAPIVersion version;
} MongoExtension;

typedef struct MongoExtensionVTable {
    MongoExtensionStatus* (*initialize)(const MongoExtension* extension,
                                        const MongoExtensionHostPortal* portal);
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
