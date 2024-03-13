/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <memory>
#include <set>
#include <string>
#include <utility>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/document_value/document_internal.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/timeseries/timeseries_gen.h"

namespace mongo::timeseries {
/**
 * Carries parameters for unpacking a bucket. The order of operations applied to determine which
 * fields are in the final document are:
 * If we are in include mode:
 *   1. Unpack all fields from the bucket.
 *   2. Remove any fields not in _fieldSet, since we are in include mode.
 *   3. Add fields from _computedMetaProjFields.
 * If we are in exclude mode:
 *   1. Unpack all fields from the bucket.
 *   2. Add fields from _computedMetaProjFields.
 *   3. Remove any fields in _fieldSet, since we are in exclude mode.
 */
class BucketSpec {
public:
    // When unpacking buckets with kInclude we must produce measurements that contain the
    // set of fields. Otherwise, if the kExclude option is used, the measurements will include the
    // set difference between all fields in the bucket and the provided fields.
    enum class Behavior { kInclude, kExclude };

    BucketSpec() = default;
    BucketSpec(std::string timeField,
               boost::optional<std::string> metaField,
               std::set<std::string> fields = {},
               Behavior behavior = Behavior::kExclude,
               std::set<std::string> computedProjections = {},
               bool usesExtendedRange = false);
    BucketSpec(const BucketSpec&);
    BucketSpec(BucketSpec&&);
    BucketSpec(const TimeseriesOptions& tsOptions);

    BucketSpec& operator=(const BucketSpec&);
    BucketSpec& operator=(BucketSpec&&);

    // The user-supplied timestamp field name specified during time-series collection creation.
    void setTimeField(std::string&& field);
    const std::string& timeField() const;
    HashedFieldName timeFieldHashed() const;

    // An optional user-supplied metadata field name specified during time-series collection
    // creation. This field name is used during materialization of metadata fields of a measurement
    // after unpacking.
    void setMetaField(boost::optional<std::string>&& field);
    const boost::optional<std::string>& metaField() const;
    boost::optional<HashedFieldName> metaFieldHashed() const;

    void setFieldSet(std::set<std::string>& fieldSet) {
        _fieldSet = std::move(fieldSet);
    }

    void addIncludeExcludeField(StringData field) {
        _fieldSet.emplace(field);
    }

    void removeIncludeExcludeField(const std::string& field) {
        _fieldSet.erase(field);
    }

    const std::set<std::string>& fieldSet() const {
        return _fieldSet;
    }

    void setBehavior(Behavior behavior) {
        _behavior = behavior;
    }

    Behavior behavior() const {
        return _behavior;
    }

    void addComputedMetaProjFields(StringData field) {
        _computedMetaProjFields.emplace(field);
    }

    const std::set<std::string>& computedMetaProjFields() const {
        return _computedMetaProjFields;
    }

    void eraseFromComputedMetaProjFields(const std::string& field) {
        _computedMetaProjFields.erase(field);
    }

    void setUsesExtendedRange(bool usesExtendedRange) {
        _usesExtendedRange = usesExtendedRange;
    }

    bool usesExtendedRange() const {
        return _usesExtendedRange;
    }

    // Returns whether 'field' depends on a pushed down $addFields or computed $project.
    bool fieldIsComputed(StringData field) const;

    // Says what to do when an event-level predicate cannot be mapped to a bucket-level predicate.
    enum class IneligiblePredicatePolicy {
        // When optimizing a query, it's fine if some predicates can't be pushed down. We'll still
        // run the predicate after unpacking, so the results will be correct.
        kIgnore,
        // When creating a partial index, it's misleading if we can't handle a predicate: the user
        // expects every predicate in the partialFilterExpression to contribute, somehow, to making
        // the index smaller.
        kError,
    };

    struct BucketPredicate {
        // A loose predicate is a predicate which returns true when any measures of a bucket
        // matches.
        std::unique_ptr<MatchExpression> loosePredicate;

        // A tight predicate is a predicate which returns true when all measures of a bucket
        // matches.
        std::unique_ptr<MatchExpression> tightPredicate;

        // Is true iff all the predicates in the original match expression are rounded by
        // 'bucketRoundingSeconds', the bucket parameters have not changed, and the predicate is not
        // a date in the "extended range".
        bool rewriteProvidesExactMatchPredicate = false;
    };

    static BucketPredicate handleIneligible(IneligiblePredicatePolicy policy,
                                            const MatchExpression* matchExpr,
                                            StringData message);

    /**
     * Takes a predicate after $_internalUnpackBucket as an argument and attempts to rewrite it as
     * new predicates on the 'control' field. There will be a 'loose' predicate that will match if
     * some of the event field matches, also a 'tight' predicate that will match if all of the event
     * field matches.
     *
     * For example, the event level predicate {a: {$gt: 5}} will generate the loose predicate
     * {control.max.a: {$_internalExprGt: 5}}. The loose predicate will be added before the
     * $_internalUnpackBucket stage to filter out buckets with no match.
     *
     * Ideally, we'd like to add a tight predicate such as {control.min.a: {$_internalExprGt: 5}} to
     * evaluate the filter on bucket level to avoid unnecessary event level evaluation. However, a
     * bucket might contain events with missing fields that are skipped when computing the controls,
     * so in reality we only add a tight predicate on timeField which is required to exist.
     *
     * If the original predicate is on the bucket's timeField we may also create a new loose
     * predicate on the '_id' field (as it incorporates min time for the bucket) to assist in index
     * utilization. For example, the predicate {time: {$lt: new Date(...)}} will generate the
     * following predicate:
     * {$and: [
     *      {_id: {$lt: ObjectId(...)}},
     *      {control.min.time: {$_internalExprLt: new Date(...)}}
     * ]}
     *
     * If the provided predicate is ineligible for this mapping and using
     * IneligiblePredicatePolicy::kIgnore, both loose and tight predicates will be set to nullptr.
     * When using IneligiblePredicatePolicy::kError it raises a user error.
     *
     * If fixedBuckets is true and the predicate is on the timeField, the generated 'loose'
     * predicate on the 'control.min.time' field will not be subtracted by 'bucketMaxSpanSeconds',
     * and some queries will not have an 'eventFilter' nor 'wholeBucketFilter'.
     */
    static BucketPredicate createPredicatesOnBucketLevelField(
        const MatchExpression* matchExpr,
        const BucketSpec& bucketSpec,
        int bucketMaxSpanSeconds,
        const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
        bool haveComputedMetaField,
        bool includeMetaField,
        bool assumeNoMixedSchemaData,
        IneligiblePredicatePolicy policy,
        bool fixedBuckets);

    /**
     * Converts an event-level predicate to a bucket-level predicate, such that
     *
     *     {$unpackBucket ...} {$match: <event-level predicate>}
     *
     * gives the same result as
     *
     *     {$match: <bucket-level predict>} {$unpackBucket ...} {$match: <event-level predicate>}
     *
     * This means the bucket-level predicate must include every bucket that might contain an event
     * matching the event-level predicate.
     *
     * This helper is used when creating a partial index on a time-series collection: logically,
     * we index only events that match the event-level partialFilterExpression, but physically we
     * index any bucket that matches the bucket-level partialFilterExpression.
     *
     * When using IneligiblePredicatePolicy::kIgnore, if the predicate can't be pushed down, it
     * returns null. When using IneligiblePredicatePolicy::kError it raises a user error.
     *
     * Returns a boolean (alongside the bucket-level predicate) describing if the result contains
     * a metric predicate.
     *
     * If fixedBuckets is true, the bounds for the bucket-level predicates can be tighter, and
     * therefore match fewer irrelevant buckets.
     */
    static std::pair<bool, BSONObj> pushdownPredicate(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const TimeseriesOptions& tsOptions,
        const BSONObj& predicate,
        bool haveComputedMetaField,
        bool includeMetaField,
        bool assumeNoMixedSchemaData,
        IneligiblePredicatePolicy policy,
        bool fixedBuckets);

    /**
     * Splits out a predicate on the meta field from a predicate on the bucket metric field.
     */
    static std::pair<std::unique_ptr<MatchExpression>, std::unique_ptr<MatchExpression>>
    splitOutMetaOnlyPredicate(std::unique_ptr<MatchExpression> expr,
                              boost::optional<StringData> metaField);

    // Used as the return value of getPushdownPredicates().
    struct SplitPredicates {
        std::unique_ptr<MatchExpression> metaOnlyExpr;
        std::unique_ptr<MatchExpression> bucketMetricExpr;
        std::unique_ptr<MatchExpression> residualExpr;
    };

    /**
     * Decomposes a predicate into three parts: a predicate on the meta field, a predicate on the
     * bucket metric field(s), and a residual predicate. The predicate on the meta field is a
     * predicate that can be evaluated on the meta field. The predicate on the bucket metric
     * field(s) is a predicate that can be evaluated on the bucket metric field(s) like
     * control.min|max.[field]. The residual predicate is a predicate that cannot be evaluated on
     * either the meta field or the bucket metric field and exactly matches desired measurements.
     */
    static SplitPredicates getPushdownPredicates(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const TimeseriesOptions& tsOptions,
        const BSONObj& predicate,
        bool haveComputedMetaField,
        bool includeMetaField,
        bool assumeNoMixedSchemaData,
        IneligiblePredicatePolicy policy,
        bool fixedBuckets);

    bool includeMinTimeAsMetadata = false;
    bool includeMaxTimeAsMetadata = false;

private:
    // The set of field names in the data region that should be included or excluded.
    std::set<std::string> _fieldSet;
    Behavior _behavior = Behavior::kExclude;

    // Set of computed meta field projection names. Added at the end of materialized
    // measurements.
    std::set<std::string> _computedMetaProjFields;

    std::string _timeField;
    boost::optional<HashedFieldName> _timeFieldHashed;

    boost::optional<std::string> _metaField = boost::none;
    boost::optional<HashedFieldName> _metaFieldHashed = boost::none;
    bool _usesExtendedRange = false;
};

/**
 * Determines if an arbitrary field should be included in the materialized measurements.
 */
inline bool determineIncludeField(StringData fieldName,
                                  BucketSpec::Behavior unpackerBehavior,
                                  const std::set<std::string>& unpackFieldsToIncludeExclude) {
    const bool isInclude = unpackerBehavior == BucketSpec::Behavior::kInclude;
    const bool unpackFieldsContains = unpackFieldsToIncludeExclude.find(fieldName.toString()) !=
        unpackFieldsToIncludeExclude.cend();
    return isInclude == unpackFieldsContains;
}
}  // namespace mongo::timeseries
