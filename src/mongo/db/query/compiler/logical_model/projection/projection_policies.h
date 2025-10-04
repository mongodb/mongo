/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

namespace mongo {

struct MONGO_MOD_NEEDS_REPLACEMENT ProjectionPolicies {
    // Allows the caller to indicate whether the projection should default to including or
    // excluding the _id field in the event that the projection spec does not specify the
    // desired behavior. For instance, given a projection {a: 1}, specifying 'kExcludeId' is
    // equivalent to projecting {a: 1, _id: 0} while 'kIncludeId' is equivalent to the
    // projection {a: 1, _id: 1}. If the user explicitly specifies a projection on _id, then
    // this will override the default policy; for instance, {a: 1, _id: 0} will exclude _id for
    // both 'kExcludeId' and 'kIncludeId'.
    enum class DefaultIdPolicy { kIncludeId, kExcludeId };

    // Allows the caller to specify how the projection should handle nested arrays; that is, an
    // array whose immediate parent is itself an array. For example, in the case of sample
    // document {a: [1, 2, [3, 4], {b: [5, 6]}]} the array [3, 4] is a nested array. The array
    // [5, 6] is not, because there is an intervening object between it and its closest array
    // ancestor.
    enum class ArrayRecursionPolicy { kRecurseNestedArrays, kDoNotRecurseNestedArrays };

    // Allows the caller to specify whether computed fields should be allowed within inclusion
    // projections. Computed fields are implicitly prohibited by exclusion projections.
    enum class ComputedFieldsPolicy {
        kBanComputedFields,
        kAllowComputedFields,
        kOnlyComputedFields
    };

    // Whether $elemMatch, find() $slice and positional projection are allowed.
    enum class FindOnlyFeaturesPolicy { kBanFindOnlyFeatures, kAllowFindOnlyFeatures };

    // Whether the empty projection, {}, is permitted.
    enum class EmptyProjectionPolicy { kBanEmptyProjection, kAllowEmptyProjection };

    static const DefaultIdPolicy kDefaultIdPolicyDefault = DefaultIdPolicy::kIncludeId;
    static const ArrayRecursionPolicy kArrayRecursionPolicyDefault =
        ArrayRecursionPolicy::kRecurseNestedArrays;
    static const ComputedFieldsPolicy kComputedFieldsPolicyDefault =
        ComputedFieldsPolicy::kAllowComputedFields;
    static const FindOnlyFeaturesPolicy kFindOnlyFeaturesPolicyDefault =
        FindOnlyFeaturesPolicy::kBanFindOnlyFeatures;
    static const EmptyProjectionPolicy kEmptyProjectionPolicyDefault =
        EmptyProjectionPolicy::kBanEmptyProjection;

    static ProjectionPolicies findProjectionPolicies() {
        return ProjectionPolicies{kDefaultIdPolicyDefault,
                                  kArrayRecursionPolicyDefault,
                                  kComputedFieldsPolicyDefault,
                                  FindOnlyFeaturesPolicy::kAllowFindOnlyFeatures,
                                  EmptyProjectionPolicy::kAllowEmptyProjection};
    }

    static ProjectionPolicies aggregateProjectionPolicies() {
        return ProjectionPolicies{kDefaultIdPolicyDefault,
                                  kArrayRecursionPolicyDefault,
                                  kComputedFieldsPolicyDefault,
                                  FindOnlyFeaturesPolicy::kBanFindOnlyFeatures,
                                  kEmptyProjectionPolicyDefault};
    }

    static ProjectionPolicies addFieldsProjectionPolicies() {
        return ProjectionPolicies{kDefaultIdPolicyDefault,
                                  kArrayRecursionPolicyDefault,
                                  ComputedFieldsPolicy::kOnlyComputedFields,
                                  FindOnlyFeaturesPolicy::kBanFindOnlyFeatures,
                                  EmptyProjectionPolicy::kAllowEmptyProjection};
    }

    static ProjectionPolicies wildcardIndexSpecProjectionPolicies() {
        return ProjectionPolicies{DefaultIdPolicy::kExcludeId,
                                  ArrayRecursionPolicy::kDoNotRecurseNestedArrays,
                                  ComputedFieldsPolicy::kBanComputedFields,
                                  FindOnlyFeaturesPolicy::kBanFindOnlyFeatures,
                                  kEmptyProjectionPolicyDefault};
    }

    ProjectionPolicies(
        DefaultIdPolicy idPolicy = kDefaultIdPolicyDefault,
        ArrayRecursionPolicy arrayRecursionPolicy = kArrayRecursionPolicyDefault,
        ComputedFieldsPolicy computedFieldsPolicy = kComputedFieldsPolicyDefault,
        FindOnlyFeaturesPolicy findOnlyFeaturesPolicy = kFindOnlyFeaturesPolicyDefault,
        EmptyProjectionPolicy emptyProjectionPolicy = kEmptyProjectionPolicyDefault)
        : idPolicy(idPolicy),
          arrayRecursionPolicy(arrayRecursionPolicy),
          computedFieldsPolicy(computedFieldsPolicy),
          findOnlyFeaturesPolicy(findOnlyFeaturesPolicy),
          emptyProjectionPolicy(emptyProjectionPolicy) {}

    const DefaultIdPolicy idPolicy;
    const ArrayRecursionPolicy arrayRecursionPolicy;
    const ComputedFieldsPolicy computedFieldsPolicy;
    const FindOnlyFeaturesPolicy findOnlyFeaturesPolicy;
    const EmptyProjectionPolicy emptyProjectionPolicy;

    bool findOnlyFeaturesAllowed() const {
        return findOnlyFeaturesPolicy == FindOnlyFeaturesPolicy::kAllowFindOnlyFeatures;
    }

    bool emptyProjectionAllowed() const {
        return emptyProjectionPolicy == EmptyProjectionPolicy::kAllowEmptyProjection;
    }
};

}  // namespace mongo
