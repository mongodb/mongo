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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/util/modules.h"

namespace mongo {

/**
 * Allocates a new, unique StageParams::Id value.
 * Assigns it to a private variable (in an anonymous namespace) based on the given `name`, and
 * declares a const reference named `constName` to the private variable.
 */
#define ALLOCATE_STAGE_PARAMS_ID(name, constName)               \
    namespace {                                                 \
    StageParams::Id _spid_##name = StageParams::kUnallocatedId; \
    MONGO_INITIALIZER_GENERAL(allocateStageId_##name,           \
                              ("BeginStageIdAllocation"),       \
                              ("EndStageIdAllocation"))         \
    (InitializerContext*) {                                     \
        _spid_##name = StageParams::allocateId(#name);          \
    }                                                           \
    }                                                           \
    const StageParams::Id& constName = _spid_##name;

/**
 * Abstract base class for parameter objects passed to aggregation pipeline stages. A stage's
 * derived implementation may contain any information calculated at lite parsing time, that is
 * useful to future representations of the stage (ie DocumentSource or Stage).
 *
 * This class provides a type-safe mechanism to identify different parameter types without using
 * RTTI (Runtime Type Information). Each concrete subclass of StageParams is assigned a unique
 * identifier at initialization time, which can be used to distinguish parameter types at runtime.
 * This allows the server to lookup the translation function for a LiteParsed->Parsed document
 * source without needing to know the specific type of stage.
 *
 * To create a new parameter type:
 *   1. Create a class that inherits from StageParams
 *   2. Use the ALLOCATE_STAGE_PARAMS_ID macro to assign a unique ID
 *   3. Implement getId() to return the allocated ID
 */
class MONGO_MOD_OPEN StageParams {
public:
    virtual ~StageParams() = default;

    /**
     * Used to identify different StageParam sub-classes, without requiring RTTI.
     */
    using Id = unsigned long;

    // Using 0 for "unallocated id" makes it easy to check if an Id has been allocated.
    static constexpr Id kUnallocatedId{0};

    virtual Id getId() const = 0;

    /**
     * Allocate and return a new, unique StageParams::Id value.
     *
     * DO NOT call this method directly. Instead, use the ALLOCATE_STAGE_PARAMS_ID macro defined
     * in this file.
     */
    static Id allocateId(StringData name);
};

/**
 * Default implementation of StageParams that stores the original BSON specification.
 *
 * This class is used as a fallback when a stage doesn't require specialized parameter handling.
 * It simply preserves the original BSONElement from the query specification, allowing stages to
 * access their original specification if needed. However, stages must still derive from this base
 * class to allocate their own ID.
 *
 * Note: This class does NOT own the backing BSON object. The BSONElement is a view into a larger
 * BSON object that must remain valid for the lifetime of this DefaultStageParams instance.
 */
class MONGO_MOD_OPEN DefaultStageParams : public StageParams {
public:
    /**
     * Constructs a DefaultStageParams from the original BSON specification element.
     *
     * @param originalSpec The BSONElement containing the original stage specification from the
     * query. This element must remain valid for the lifetime of this object.
     */
    DefaultStageParams(BSONElement originalSpec);

    /**
     * Returns the original BSON specification element that was provided during construction.
     *
     * @return A BSONElement view into the original specification. The backing BSON object must
     *         remain valid for the lifetime of this DefaultStageParams instance.
     */
    BSONElement getOriginalBson() const;

private:
    // The original spec that was provided in the query. Note that these params do NOT own the
    // backing BSON object.
    BSONElement _originalSpec;
};

/**
 * Defines a stage-specific parameter class that uses the default implementation.
 *
 * This macro creates a new class named `{stageName}StageParams` that inherits from
 * `DefaultStageParams`. The class stores the original BSON specification element and
 * provides a unique type identifier for runtime type checking without using RTTI.
 *
 * Use this macro when a stage needs its own parameter type for identification purposes
 * but doesn't require custom parameter parsing or storage beyond the default behavior
 * (which simply preserves the original BSONElement).
 *
 * @param stageName The name of the stage (without the "$" prefix). This will be used
 *                  to generate the class name `{stageName}StageParams` and to allocate
 *                  a unique ID for type identification.
 *
 * Example usage:
 *   DECLARE_STAGE_PARAMS_DERIVED_DEFAULT(Test);
 *   // Creates TestStageParams class that can be instantiated with:
 *   // auto params = std::make_unique<TestStageParams>(bsonElement);
 */
#define DECLARE_STAGE_PARAMS_DERIVED_DEFAULT(stageName)                                            \
    class stageName##StageParams : public mongo::DefaultStageParams {                              \
    public:                                                                                        \
        stageName##StageParams(mongo::BSONElement element) : mongo::DefaultStageParams(element) {} \
        static const Id& id;                                                                       \
        Id getId() const final {                                                                   \
            return id;                                                                             \
        }                                                                                          \
    };

}  // namespace mongo
