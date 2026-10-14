// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/ce/sampling/sampling_estimator_impl.h"

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/db/commands/server_status/server_status_metric.h"
#include "mongo/db/exec/matcher/matcher.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/namespace_string_util.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/compiler/ce/ce_common.h"
#include "mongo/db/query/compiler/ce/ndv/field_stats.h"
#include "mongo/db/query/compiler/ce/ndv/field_stats_loader.h"
#include "mongo/db/query/compiler/ce/sampling/math.h"
#include "mongo/db/query/compiler/ce/sampling/sample_manager.h"
#include "mongo/db/query/compiler/dependency_analysis/match_expression_dependencies.h"
#include "mongo/db/query/compiler/optimizer/cost_based_ranker/estimates.h"
#include "mongo/db/query/query_feature_flags_gen.h"
#include "mongo/db/query/query_knobs/query_knob_configuration.h"
#include "mongo/db/query/query_optimization_knobs_gen.h"
#include "mongo/db/query/stage_builder/sbe/builder.h"
#include "mongo/db/server_options.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/version_context.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"

#include <algorithm>
#include <string_view>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQueryCE

namespace mongo::ce {
using namespace std::literals::string_view_literals;

using CardinalityType = mongo::cost_based_ranker::CardinalityType;
using EstimationSource = mongo::cost_based_ranker::EstimationSource;

MONGO_FAIL_POINT_DEFINE(hangBeforeCBRSamplingGenerateSample);
MONGO_FAIL_POINT_DEFINE(hangAfterCBRSamplingGenerateSample);

namespace {

/**
 * Helper function to determine whether a given set of field names contains a
 * dotted path.
 */
bool containsDottedPath(const StringSet& topLevelSampleFieldNames) {
    return std::any_of(
        topLevelSampleFieldNames.begin(),
        topLevelSampleFieldNames.end(),
        [](const std::string& fieldName) { return fieldName.find('.') != std::string::npos; });
}

void validateTopLevelSampleFieldNames(const StringSet& topLevelSampleFieldNames) {
    tassert(10770100,
            "topLevelSampleFieldNames must be a non-empty set if specified.",
            !topLevelSampleFieldNames.empty());
    tassert(10670300,
            "topLevelSampleFieldNames should not contain a dotted field path",
            !containsDottedPath(topLevelSampleFieldNames));
}

/**
 * Helper function to determine if a given MatchExpression contains only fields present in the
 * sample.
 */
void checkSampleContainsMatchExpressionFields(const StringSet& topLevelSampleFieldNames,
                                              const MatchExpression* expr) {
    const auto matchExpressionFields = extractTopLevelFieldsFromMatchExpression(expr);
    tassert(13452601,
            "MatchExpression needs the whole document but the sample doesn't contain all fields",
            !matchExpressionFields.needsAllFields());

    for (const auto& matchField : matchExpressionFields.fieldNames()) {
        tassert(10670301,
                "MatchExpression contains fields not present in topLevelSampleFieldNames. "
                "MatchExpression: " +
                    expr->toString(),
                topLevelSampleFieldNames.contains(matchField));
    }
}

void checkSampleContainsIndexBoundsFields(const StringSet& topLevelSampleFieldNames,
                                          const IndexBounds& bounds) {
    for (auto& oil : bounds.fields) {
        tassert(10770101,
                "Field in index bounds should be included in the set of sampled fields.",
                topLevelSampleFieldNames.contains(stage_builder::getTopLevelField(oil.name)));
    }
}

/**
 * Tries to evaluate 'expr' against 'doc'. Returns true/false for match/no-match, or boost::none if
 * evaluation threw a DBException.
 */
boost::optional<bool> tryMatchesBSON(const MatchExpression* expr, const BSONObj& doc) {
    try {
        return exec::matcher::matchesBSON(expr, doc, nullptr);
    } catch (const DBException&) {
        return boost::none;
    }
}

/**
 * Computes a scaled cardinality estimate from the given match count and error count. The
 * selectivity is computed over only the successfully evaluated documents (i.e., the effective
 * sample size is 'sampleSize - errorCount').
 */
CardinalityEstimate makeScaledEstimate(double matchCount,
                                       size_t errorCount,
                                       size_t sampleSize,
                                       CardinalityEstimate collCard,
                                       bool wasSamplePersisted) {
    size_t effectiveSampleSize = sampleSize - errorCount;
    double estimate =
        effectiveSampleSize > 0 ? (matchCount * collCard.toDouble()) / effectiveSampleSize : 0.0;
    // The estimate is authoritative (tagged 'Code' rather than 'Sampling') only when the
    // sample was freshly collected on the fly and either:
    // * the expression errored for every sample document - the zero is not an approximation
    // * the sample covers the entire collection - 'matchCount' is the true match count and
    // 'estimate' is exact.
    // Tagging these as Code prevents CardinalityEstimator::clampZeroEstimates from inflating
    // the estimates.

    if (wasSamplePersisted) {
        return CardinalityEstimate{CardinalityType{estimate}, EstimationSource::Sampling};
    }
    const bool isAuthoritative =
        effectiveSampleSize == 0 || static_cast<double>(effectiveSampleSize) >= collCard.toDouble();
    return CardinalityEstimate{CardinalityType{estimate},
                               isAuthoritative ? EstimationSource::Code
                                               : EstimationSource::Sampling};
}

}  // namespace

TopLevelSampleFields extractTopLevelFieldsFromMatchExpression(const MatchExpression* expr) {
    DepsTracker deps;
    dependency_analysis::addDependencies(expr, &deps);
    if (deps.needWholeDocument) {
        return TopLevelSampleFields::allFields();
    }

    StringSet topLevelFieldsSet;
    for (auto&& path : deps.fields) {
        const auto field = stage_builder::getTopLevelField(path);
        topLevelFieldsSet.emplace(std::string(field));
    }
    return TopLevelSampleFields{std::move(topLevelFieldsSet)};
}

const StringSet& TopLevelSampleFields::fieldNames() const {
    tassert(13452602, "All fields are needed", !needsAllFields());
    return _fieldNames.value();
}

void TopLevelSampleFields::merge(TopLevelSampleFields other) {
    if (needsAllFields() || other.needsAllFields()) {
        _fieldNames = kAllFields;
    } else {
        _fieldNames->merge(other._fieldNames.value());
    }
}

boost::optional<StringSet&> TopLevelSampleFields::relevantIndexOutput() {
    return !needsAllFields() ? boost::optional<StringSet&>(_fieldNames.value()) : boost::none;
}

ProjectionParams TopLevelSampleFields::toProjectionParams() && {
    if (needsAllFields() || _fieldNames->empty()) {
        return NoProjection{};
    }
    return TopLevelFieldsProjection{std::move(_fieldNames.value())};
}

void SamplingEstimatorImpl::generateSample(ce::ProjectionParams projectionParams) {
    tassert(12433201, "SamplingEstimatorImpl must not be reused", !_sample.has_value());

    if (auto topLevelSampleFieldNames =
            std::get_if<ce::TopLevelFieldsProjection>(&projectionParams)) {
        validateTopLevelSampleFieldNames(*topLevelSampleFieldNames);
        _spec.topLevelSampleFieldNames = *topLevelSampleFieldNames;
    }

    // Test hook: pause here so tests can arm setYieldAllLocksHang after any multiplanning
    // trial phase is done, ensuring the yield fires inside the sampling executor.
    hangBeforeCBRSamplingGenerateSample.executeIf(
        [&](const BSONObj&) { hangBeforeCBRSamplingGenerateSample.pauseWhileSet(_opCtx); },
        [&](const BSONObj& data) {
            const auto fpNss = NamespaceStringUtil::parseFailPointData(data, "collectionNS");
            return fpNss.isEmpty() || _nss == fpNss;
        });

    SampleManager sampleManager(_opCtx, _yieldPolicy, _customerQueryExpCtx);

    _sample = sampleManager.load(_collections, _spec);
    _uniqueDocCount = boost::none;
    hangAfterCBRSamplingGenerateSample.executeIf(
        [&](const BSONObj&) { hangAfterCBRSamplingGenerateSample.pauseWhileSet(_opCtx); },
        [&](const BSONObj& data) {
            const auto fpNss = NamespaceStringUtil::parseFailPointData(data, "collectionNS");
            return fpNss.isEmpty() || _nss == fpNss;
        });
}

namespace {

/**
 * If 'expr' is an AndMatchExpression whose children are all EqualityMatchExpressions on the same
 * field path without a collator, returns {path, values}. Otherwise returns boost::none.
 *
 * This is the common shape produced when parsing $all
 */
boost::optional<std::pair<std::string_view, std::vector<BSONElement>>> tryExtractAllEqualities(
    const MatchExpression* expr) {
    if (expr->matchType() != MatchExpression::AND || expr->numChildren() == 0) {
        return boost::none;
    }

    // Because the CanonicalQuery normalizer sorts $and children by match type (contiguously) and
    // then by path within the same type, we can use the first and last children to check for some
    // early return conditions:
    //   - If not all children are EQ, then either/both of these children will not be EQ.
    //   - If not all children share the same path, then the first and last children will have
    //   different paths.
    //   - This optimization won't work if the common path is a dotted path.
    // Either mismatch means this $and cannot be the all-equalities fast path.
    const auto* first = expr->getChild(0);
    const auto* last = expr->getChild(expr->numChildren() - 1);
    if (first->matchType() != MatchExpression::EQ || last->matchType() != MatchExpression::EQ) {
        return boost::none;
    }
    if (first->path() != last->path()) {
        return boost::none;
    }
    if (first->path().find('.') != std::string::npos) {
        return boost::none;
    }

    // All children share the same path (verified by the check above).
    // We still need to scan every child for collation and null/undefined rejections.
    std::string_view commonPath = static_cast<const EqualityMatchExpression*>(first)->path();
    std::vector<BSONElement> values;
    values.reserve(expr->numChildren());

    for (size_t i = 0; i < expr->numChildren(); ++i) {
        const auto* child = expr->getChild(i);

        // Not all MatchExpressions are normalized, so we still have to double check that the
        // expression is an EQ with the same common path.
        if (child->matchType() != MatchExpression::EQ || child->path() != commonPath) {
            return boost::none;
        }

        const auto* eq = static_cast<const EqualityMatchExpression*>(child);
        // Reject collation-sensitive equalities: collator affects comparison semantics.
        if (eq->getCollator() != nullptr) {
            return boost::none;
        }
        // Reject null/undefined equalities: EqualityMatchExpression has special "null ==
        // missing" semantics for these types that our simple value comparison doesn't replicate.
        const BSONElement& rhs = eq->getData();
        if (rhs.isNull() || rhs.type() == BSONType::undefined) {
            return boost::none;
        }
        values.push_back(rhs);
    }

    return std::make_pair(commonPath, std::move(values));
}

/**
 * Returns true if 'doc' has a field at 'path' that contains ALL values in 'sortedRequiredValues'
 * (which must already be sorted by BSONElementCmpWithoutField order. Note that deduplication is not
 * necessary because $all queries are deduplicated during boolean simplification).
 *
 * For array-valued fields, uses a merge-scan to check all values are contained in the field,
 * iterating through each vector once.
 */
bool documentMatchesAllEqualities(const BSONObj& doc,
                                  std::string_view path,
                                  const std::vector<BSONElement>& sortedRequiredValues) {
    BSONElementCmpWithoutField cmp;

    BSONElement fieldElem = doc.getField(path);
    if (fieldElem.eoo()) {
        return false;
    }

    // Collect the document field's values into a sorted vector for merge-scan.
    std::vector<BSONElement> docValues;
    if (fieldElem.type() == BSONType::array) {
        BSONObjIterator iter(fieldElem.embeddedObject());
        while (iter.more()) {
            docValues.push_back(iter.next());
        }
    } else {
        docValues.push_back(fieldElem);
    }
    std::sort(docValues.begin(), docValues.end(), cmp);

    return std::includes(docValues.begin(),
                         docValues.end(),
                         sortedRequiredValues.begin(),
                         sortedRequiredValues.end(),
                         cmp);
}

}  // namespace

CardinalityEstimate SamplingEstimatorImpl::estimateCardinality(const MatchExpression* expr) const {
    tassert(10981500,
            "Sample must be generated before calling estimateCardinality()",
            _sample.has_value());
    if (_sample->size() == 0) {
        return cost_based_ranker::zeroCE;
    }

    if (!_spec.topLevelSampleFieldNames.empty()) {
        checkSampleContainsMatchExpressionFields(_spec.topLevelSampleFieldNames, expr);
    }

    size_t cnt = 0;
    size_t errorCount = 0;

    // Fast path for AND of equalities on the same field path (the common shape produced by $all).
    // The generic matcher evaluates each equality by scanning the array independently: O(N *
    // array_length) per sample document. This fast path sorts the required values once and uses a
    // merge-scan per document.
    if (auto allEqInfo = tryExtractAllEqualities(expr)) {
        auto& [path, requiredValues] = *allEqInfo;
        BSONElementCmpWithoutField cmp;
        // Sort required values
        std::sort(requiredValues.begin(), requiredValues.end(), cmp);
        for (const auto& doc : _sample->docs()) {
            if (documentMatchesAllEqualities(doc, path, requiredValues)) {
                ++cnt;
            }
        }
    } else {
        for (const auto& doc : _sample->docs()) {
            auto result = tryMatchesBSON(expr, doc);
            if (!result) {
                errorCount++;
                continue;
            }
            if (*result) {
                cnt++;
            }
        }
    }

    CardinalityEstimate estimate =
        makeScaledEstimate(cnt, errorCount, _sample->size(), getCollCard(), _sample->isPersisted());

    LOGV2_DEBUG(9756604,
                5,
                "SamplingCE cardinality (# docs) for MatchExpression",
                "matchExpression"_attr = expr->toString(),
                "estimate"_attr = estimate);
    return estimate;
}


CardinalityEstimate SamplingEstimatorImpl::estimateKeysScanned(const IndexBounds& bounds) const {
    tassert(10981502,
            "Sample must be generated before calling estimateKeysScanned()",
            _sample.has_value());
    // An empty sample (e.g. an empty collection) means zero keys were scanned. Return early to
    // avoid the divide-by-'_sample->size()' below. Mirrors the guard in estimateCardinality().
    if (_sample->size() == 0) {
        return cost_based_ranker::zeroCE;
    }
    if (bounds.isSimpleRange) {
        MONGO_UNIMPLEMENTED_TASSERT(9811500);
    }
    if (!_spec.topLevelSampleFieldNames.empty()) {
        checkSampleContainsIndexBoundsFields(_spec.topLevelSampleFieldNames, bounds);
    }

    size_t count = 0;

    forNumberKeysMatch(bounds, _sample->docs(), [&](size_t matchCnt) { count += matchCnt; });

    CardinalityEstimate estimate{
        CardinalityType{(count * getCollCard().toDouble()) / _sample->size()},
        EstimationSource::Sampling};
    LOGV2_DEBUG(9756605,
                5,
                "SamplingCE cardinality (# keys) for index bounds",
                "interval"_attr = bounds.toString(false),
                "estimate"_attr = estimate);
    return estimate;
}

std::vector<CardinalityEstimate> SamplingEstimatorImpl::estimateKeysScanned(
    const std::vector<const IndexBounds*>& bounds) const {
    tassert(10981503,
            "Sample must be generated before calling estimateKeysScanned()",
            _sample.has_value());
    std::vector<CardinalityEstimate> estimates;
    estimates.reserve(bounds.size());
    for (size_t i = 0; i < bounds.size(); i++) {
        estimates.push_back(estimateKeysScanned(*bounds[i]));
    }
    return estimates;
}

std::vector<CardinalityEstimate> SamplingEstimatorImpl::estimateRIDs(
    const std::vector<const IndexBounds*>& bounds,
    const std::vector<const MatchExpression*>& expressions) const {
    tassert(
        10981504, "Sample must be generated before calling estimateRIDs()", _sample.has_value());

    std::vector<CardinalityEstimate> estimates;
    tassert(9942301,
            "bounds and expressions should have equal size.",
            expressions.size() == bounds.size());
    estimates.reserve(bounds.size());
    for (size_t i = 0; i < bounds.size(); i++) {
        estimates.push_back(estimateRIDs(*bounds[i], expressions[i]));
    }
    return estimates;
}

CardinalityEstimate SamplingEstimatorImpl::estimateRIDs(const IndexBounds& bounds,
                                                        const MatchExpression* expr) const {
    tassert(
        10981505, "Sample must be generated before calling estimateRIDs()", _sample.has_value());
    if (!_spec.topLevelSampleFieldNames.empty()) {
        checkSampleContainsMatchExpressionFields(_spec.topLevelSampleFieldNames, expr);
        checkSampleContainsIndexBoundsFields(_spec.topLevelSampleFieldNames, bounds);
    }
    // Precompute the fast-path info for AND-of-same-path-equalities outside the bounds loop.
    boost::optional<std::pair<std::string_view, std::vector<BSONElement>>> allEqInfo;
    std::vector<BSONElement> sortedRequiredValues;
    if (expr) {
        allEqInfo = tryExtractAllEqualities(expr);
        if (allEqInfo) {
            BSONElementCmpWithoutField cmp;
            sortedRequiredValues = allEqInfo->second;
            std::sort(sortedRequiredValues.begin(), sortedRequiredValues.end(), cmp);
        }
    }

    size_t count = 0;
    size_t errorCount = 0;
    forDocumentsMatchingBounds(bounds, _sample->docs(), [&](const BSONObj& document) {
        // If 'expr' is null, we are simply estimating the cardinality by IndexBounds.
        if (expr == nullptr) {
            count++;
            return true;
        } else if (allEqInfo) {
            if (documentMatchesAllEqualities(document, allEqInfo->first, sortedRequiredValues)) {
                count++;
            }
            return true;
        } else {
            auto result = tryMatchesBSON(expr, document);
            if (!result) {
                errorCount++;
                return true;
            }
            if (*result) {
                count++;
            }
            return true;
        }
    });
    CardinalityEstimate estimate = makeScaledEstimate(
        count, errorCount, _sample->size(), getCollCard(), _sample->isPersisted());
    LOGV2_DEBUG(9756606,
                5,
                "SamplingCE cardinality (# docs) for index bounds and MatchExpression",
                "interval"_attr = bounds.toString(false),
                "matchExpression"_attr = expr ? expr->toString() : "<none>",
                "estimate"_attr = estimate);
    return estimate;
}

SamplingEstimatorImpl::SamplingEstimatorImpl(
    OperationContext* opCtx,
    const MultipleCollectionAccessor& collections,
    const NamespaceString& nss,
    PlanYieldPolicy::YieldPolicy yieldPolicy,
    size_t sampleSize,
    SamplingCEMethodEnum onTheFlySamplingMethod,
    boost::optional<int> numChunks,
    CardinalityEstimate collectionCard,
    boost::intrusive_ptr<const ExpressionContext> customerQueryExpCtx,
    SamplingSourceEnum samplingSource,
    SamplingCEMethodEnum persistentSamplingMethod)
    : _spec{.nss = nss,
            .sampleSize = sampleSize,
            .onTheFlySamplingMethod = onTheFlySamplingMethod,
            .persistentSamplingMethod = persistentSamplingMethod,
            .numChunks = numChunks,
            .samplingSource = samplingSource,
            .topLevelSampleFieldNames = {}},
      _collectionCard(collectionCard),
      _opCtx(opCtx),
      _collections(collections),
      _customerQueryExpCtx(std::move(customerQueryExpCtx)),
      _nss(nss),
      _yieldPolicy(yieldPolicy) {
    tassert(12432804,
            "numChunks must be positive when provided",
            !_spec.numChunks || *_spec.numChunks > 0);
}

SamplingEstimatorImpl::SamplingEstimatorImpl(
    OperationContext* opCtx,
    const MultipleCollectionAccessor& collections,
    const NamespaceString& nss,
    PlanYieldPolicy::YieldPolicy yieldPolicy,
    size_t sampleSize,
    SamplingCEMethodEnum onTheFlySamplingMethod,
    boost::optional<int> numChunks,
    long long numRecords,
    boost::intrusive_ptr<const ExpressionContext> customerQueryExpCtx,
    SamplingSourceEnum samplingSource)
    : SamplingEstimatorImpl(opCtx,
                            collections,
                            nss,
                            yieldPolicy,
                            sampleSize,
                            onTheFlySamplingMethod,
                            numChunks,
                            CardinalityEstimate{CardinalityType{static_cast<double>(numRecords)},
                                                EstimationSource::Metadata},
                            std::move(customerQueryExpCtx),
                            samplingSource) {}

SamplingEstimatorImpl::~SamplingEstimatorImpl() {}

namespace {
// Hit = served an NDV from persisted field statistics. Miss = fell back to the sample-based
// estimate although persisted NDV statistics were enabled and requested. Counted once per
// estimator, canonical path set and folding variant; memoized re-reads within one estimator do
// not count again.
auto& persistentNDVHits = *MetricBuilder<Counter64>{"query.sampling.persistentNdv.hits"};
auto& persistentNDVMisses = *MetricBuilder<Counter64>{"query.sampling.persistentNdv.misses"};
}  // namespace

SamplingMetadata SamplingEstimatorImpl::getSamplingMetadata() const {
    tassert(
        12433200, "getSamplingMetadata() called before sample was generated", _sample.has_value());
    // Account for: vector metadata, BSONObj object overhead per slot, and per-document
    // buffer allocation (BSON data + SharedBuffer::Holder ref-count header).
    size_t memorySizeBytes =
        sizeof(std::vector<BSONObj>) + sizeof(BSONObj) * _sample->docs().capacity();
    for (const auto& doc : _sample->docs()) {
        tassert(12433202, "Sample documents must be owned BSONObjs", doc.isOwned());
        // TODO SERVER-126975. Read this from the persisted doc.
        memorySizeBytes += SharedBuffer::kHolderSize + static_cast<size_t>(doc.objsize());
    }
    SamplingMetadata meta;
    meta.isPersisted = _sample->isPersisted();
    meta.docCount = _sample->size();
    meta.requestedDocCount = _spec.sampleSize;
    meta.memorySizeBytes = memorySizeBytes;
    meta.technique = _sample->technique();
    if (_sample->technique() == SamplingTechniqueEnum::kChunk) {
        meta.numChunks = _spec.numChunks;
    }
    meta.createdAt = _sample->createdAt();
    meta.numPages = _sample->numPages();
    return meta;
}

std::vector<PersistedNDVEntry> SamplingEstimatorImpl::getPersistedNDVMetadata() const {
    auto entries = _persistedNDVStatsUsed;
    // Insertion order follows optimizer traversal; sort for deterministic explain output.
    // std::vector's operator< compares lexicographically, element by element, so entries order
    // by their first differing path, with a shorter path list before its extensions.
    std::sort(entries.begin(), entries.end(), [](auto& a, auto& b) {
        return a.sortedFieldPaths < b.sortedFieldPaths;
    });
    return entries;
}

bool SamplingEstimatorImpl::persistentNDVStatsEnabled() const {
    if (!_persistentNDVEnabled) {
        const bool flagEnabled = feature_flags::gFeatureFlagPersistentStats.isEnabled(
            VersionContext::getDecoration(_opCtx),
            serverGlobalParams.featureCompatibility.acquireFCVSnapshot());
        // The operation's knob configuration, which reflects any query settings overrides.
        _persistentNDVEnabled =
            flagEnabled && QueryKnobConfiguration::get(_opCtx).getEnablePersistentNDVStats();
    }
    return *_persistentNDVEnabled;
}

boost::optional<size_t> SamplingEstimatorImpl::selectPersistedNDVSketchIndex(
    const std::vector<std::pair<std::string, bool>>& sortedFields,
    const SortedFieldPaths& sortedPaths) const {
    // A single-field statistic persists exactly one sketch; it serves requests of either
    // equality semantics (the two answers differ by at most one distinct value). A composite
    // statistic persists sketches[0], where every field counts null and missing separately,
    // plus sketches[i + 1] for each sorted field i, where that one field counts null and
    // missing as the same value. No sketch merges null and missing for more than one field.
    if (sortedFields.size() == 1) {
        return 0;
    }

    size_t foldedCount = 0;
    size_t sketchIndex = 0;
    for (size_t i = 0; i < sortedFields.size(); ++i) {
        if (/*isExprEq=*/!sortedFields[i].second) {
            ++foldedCount;
            sketchIndex = i + 1;
        }
    }
    if (foldedCount > 1) {
        // No persisted variant folds several fields; the caller falls back to the sample.
        LOGV2_DEBUG(13176600,
                    5,
                    "Persisted NDV statistics cannot serve a request folding several fields",
                    logAttrs(_nss),
                    "paths"_attr = sortedPaths,
                    "numFoldedFields"_attr = foldedCount);
        return boost::none;
    }
    // When no field is folded, 'sketchIndex' was never assigned and still selects the strict
    // tuple sketch at index 0.
    return sketchIndex;
}

const boost::optional<CardinalityEstimate>* PersistedNDVStatsCache::findEstimate(
    const SortedFieldPaths& sortedPaths, size_t sketchIndex) const {
    const auto it = _estimates.find(std::pair(sortedPaths, sketchIndex));
    return it == _estimates.end() ? nullptr : &it->second;
}

void PersistedNDVStatsCache::storeEstimate(const SortedFieldPaths& sortedPaths,
                                           size_t sketchIndex,
                                           boost::optional<CardinalityEstimate> estimate) {
    _estimates.emplace(std::pair(sortedPaths, sketchIndex), std::move(estimate));
}

const boost::optional<PersistedNDVEntry>& PersistedNDVStatsCache::getOrLoadDoc(
    const SortedFieldPaths& sortedPaths,
    const std::function<boost::optional<PersistedNDVEntry>()>& load) {
    if (const auto it = _docs.find(sortedPaths); it != _docs.end()) {
        return it->second;
    }
    return _docs.emplace(sortedPaths, load()).first->second;
}

boost::optional<PersistedNDVEntry> SamplingEstimatorImpl::loadPersistedNDVStats(
    const SortedFieldPaths& sortedPaths, size_t numFields) const {
    const CollectionPtr& collection = _collections.lookupCollection(_nss);
    if (!collection) {
        return boost::none;
    }

    auto swDoc = loadFieldStats(_opCtx, _nss.dbName(), collection->uuid(), sortedPaths);
    if (!swDoc.isOK()) {
        LOGV2_DEBUG(13176000,
                    5,
                    "Found no usable persisted NDV statistics",
                    logAttrs(_nss),
                    "paths"_attr = sortedPaths,
                    "reason"_attr = swDoc.getStatus());
        return boost::none;
    }

    // The _id lookup encodes the identity, but the top-level copies of these fields come
    // from storage all the same; treat a document whose copies disagree as unusable
    // instead of trusting it.
    const auto& docPaths = swDoc.getValue().getSortedFieldPaths();
    const bool identityMatches = swDoc.getValue().getSchemaVersion() == kFieldStatsSchemaVersion &&
        swDoc.getValue().getCollectionUuid() == collection->uuid() &&
        std::equal(docPaths.begin(), docPaths.end(), sortedPaths.begin(), sortedPaths.end());
    if (!identityMatches) {
        LOGV2_DEBUG(13176601,
                    5,
                    "Ignoring persisted NDV statistics document with inconsistent "
                    "identity fields",
                    logAttrs(_nss),
                    "paths"_attr = sortedPaths);
        return boost::none;
    }

    // A single-field statistic carries exactly one sketch, a composite one carries one
    // per folding variant. Any other shape was written by a version that disagrees with
    // ours, so fall back to the sample rather than asserting on stored data.
    const auto& sketches = swDoc.getValue().getNdv().getSketches();
    const size_t expectedNumSketches = numFields == 1 ? 1 : numFields + 1;
    if (sketches.size() != expectedNumSketches) {
        LOGV2_DEBUG(13176002,
                    5,
                    "Ignoring unusable persisted NDV statistics document",
                    logAttrs(_nss),
                    "paths"_attr = sortedPaths,
                    "expectedNumSketches"_attr = expectedNumSketches,
                    "numSketches"_attr = sketches.size());
        return boost::none;
    }

    // Keep only what serving needs; the sketch registers (16KB each) are dropped here
    // rather than staying resident in the planner for its lifetime.
    PersistedNDVEntry summary;
    summary.sortedFieldPaths = sortedPaths;
    summary.createdAt = swDoc.getValue().getCreatedAt();
    summary.ndvPerSketch.reserve(sketches.size());
    for (const auto& sketch : sketches) {
        summary.ndvPerSketch.push_back(sketch.getNdv());
    }
    return summary;
}

boost::optional<CardinalityEstimate> SamplingEstimatorImpl::tryEstimateNDVFromPersistentStats(
    const std::vector<FieldPathAndEqSemantics>& fields) const {
    if (fields.empty() || fields.size() > ce::kNdvMaxFields || !persistentNDVStatsEnabled()) {
        return boost::none;
    }

    // NDV(a, b) == NDV(b, a): statistics are keyed on canonically sorted paths, so sort a copy
    // of the request. The caller's order stays untouched for the sample-based fallback.
    std::vector<std::pair<std::string, bool>> sortedFields;
    sortedFields.reserve(fields.size());
    for (const auto& field : fields) {
        sortedFields.emplace_back(field.path.fullPath(), field.isExprEq);
    }
    std::sort(sortedFields.begin(), sortedFields.end());

    const size_t numFields = sortedFields.size();
    SortedFieldPaths sortedPaths;
    sortedPaths.reserve(numFields);
    for (const auto& [path, _] : sortedFields) {
        sortedPaths.push_back(path);
    }

    const auto maybeSketchIndex = selectPersistedNDVSketchIndex(sortedFields, sortedPaths);
    if (!maybeSketchIndex) {
        return boost::none;
    }
    const size_t sketchIndex = *maybeSketchIndex;

    if (const auto* cached = _persistedNDVCache.findEstimate(sortedPaths, sketchIndex)) {
        return *cached;
    }

    // Load the statistics document once per path set; further variants of the same statistic
    // are served from the cached summary.
    const boost::optional<PersistedNDVEntry>& doc = _persistedNDVCache.getOrLoadDoc(
        sortedPaths, [&] { return loadPersistedNDVStats(sortedPaths, numFields); });

    const auto estimate = [&]() -> boost::optional<CardinalityEstimate> {
        if (!doc) {
            return boost::none;
        }

        // The selected variant must hold a non-negative NDV.
        const auto ndv = doc->ndvPerSketch[sketchIndex];
        if (ndv < 0) {
            LOGV2_DEBUG(13176602,
                        5,
                        "Ignoring persisted NDV statistics with a negative NDV",
                        logAttrs(_nss),
                        "paths"_attr = sortedPaths,
                        "ndv"_attr = ndv);
            return boost::none;
        }

        CardinalityEstimate persistedEstimate{CardinalityType{static_cast<double>(ndv)},
                                              EstimationSource::HyperLogLog};
        // The statistics may predate writes; never serve more than the current collection
        // cardinality (the sample-based path below clamps the same way). The capped value keeps
        // its HyperLogLog source: callers can use the source to tell how the NDV was obtained.
        if (cost_based_ranker::exactGt(persistedEstimate, _collectionCard)) {
            persistedEstimate = {_collectionCard.cardinality(), EstimationSource::HyperLogLog};
        }
        // Surfaced in explain as queryPlanner.fieldStatsMetadata.<ns>.ndv, one entry per
        // statistics document, however many of its variants were served.
        const bool alreadyRecorded =
            std::any_of(_persistedNDVStatsUsed.begin(),
                        _persistedNDVStatsUsed.end(),
                        [&](const auto& entry) { return entry.sortedFieldPaths == sortedPaths; });
        if (!alreadyRecorded) {
            _persistedNDVStatsUsed.push_back(*doc);
        }
        LOGV2_DEBUG(13176001,
                    5,
                    "Serving NDV from persisted statistics",
                    logAttrs(_nss),
                    "paths"_attr = sortedPaths,
                    "estimate"_attr = persistedEstimate);
        return persistedEstimate;
    }();

    (estimate ? persistentNDVHits : persistentNDVMisses).incrementRelaxed();
    _persistedNDVCache.storeEstimate(sortedPaths, sketchIndex, estimate);
    return estimate;
}

CardinalityEstimate SamplingEstimatorImpl::estimateNDV(
    const std::vector<FieldPathAndEqSemantics>& fields,
    boost::optional<std::span<const OrderedIntervalList>> bounds) const {
    tassert(11158504, "Sample must be generated before calling estimateNDV()", _sample.has_value());

    // Bounded NDV stays sample-only by design: a whole-domain sketch cannot answer how many
    // distinct values fall within a range.
    if (!bounds) {
        if (auto persisted = tryEstimateNDVFromPersistentStats(fields)) {
            return *persisted;
        }
    }

    // Only the sample-based path below reads the field out of the sample, so this precondition is
    // checked after the persisted statistics have had their chance to serve the estimate.
    if (!_spec.topLevelSampleFieldNames.empty()) {
        for (const auto& field : fields) {
            tassert(11158505,
                    "Sample must include the NDV fieldName as a top-level field.",
                    _spec.topLevelSampleFieldNames.contains(field.path.front()));
        }
    }

    // Obtain the NDV for the sample. Compare against the number of unique documents. If they are
    // equal, skip NR iteration since it is likely to diverge. The best guess is that each element
    // in the collection is unique. We compare sampleNDV against uniqueDocCount instead of
    // sampleSize because we sample with replacement. This can produce duplicate documents, which
    // may result in us oscillating in and out of NR for defacto unique fields. The unique doc count
    // is lazily computed and cached.
    size_t sampleNDV = countNDV(fields, _sample->docs(), bounds);
    if (!_uniqueDocCount) {
        // TODO SERVER-135986: Fix unique document count for projected samples
        _uniqueDocCount = countUniqueDocuments(_sample->docs());
    }
    if (sampleNDV == *_uniqueDocCount) {
        LOGV2_DEBUG(11228302,
                    5,
                    "SamplingCE NDV is equal to the unique document count, outputting collection "
                    "size",
                    "fields"_attr = fields,
                    "sampleNDV"_attr = sampleNDV,
                    "uniqueDocCount"_attr = *_uniqueDocCount,
                    "sampleSize"_attr = _sample->size(),
                    "collectionCard"_attr = _collectionCard);
        return {_collectionCard.cardinality(), EstimationSource::Sampling};
    }

    tassert(12237901,
            "Non-bounded NDV count should never be zero",
            bounds.has_value() || sampleNDV != 0);
    if (bounds.has_value() && sampleNDV == 0) {
        return cost_based_ranker::zeroCE;
    }

    // Note that we use sample size instead of '_uniqueDocCount' here because the method of
    // moments estimator that we use assumes that we perform sampling with replacement.
    // TODO SERVER-135994: NDV with bounds on sample is not scaled
    CardinalityEstimate estimate = newtonRaphsonNDV(sampleNDV, _sample->size());
    LOGV2_DEBUG(11158506,
                5,
                "SamplingCE ndv (# unique values) for field",
                "fields"_attr = fields,
                "sampleNDV"_attr = sampleNDV,
                "estimate"_attr = estimate);

    if (cost_based_ranker::exactGt(estimate, _collectionCard)) {
        LOGV2_DEBUG(11158507,
                    5,
                    "SamplingCE ndv exceeds collection size, rounding down",
                    "fields"_attr = fields,
                    "sampleNDV"_attr = sampleNDV,
                    "estimate"_attr = estimate,
                    "collectionCard"_attr = _collectionCard);
        estimate = {_collectionCard.cardinality(), EstimationSource::Sampling};
    }
    return estimate;
}

CardinalityEstimate SamplingEstimatorImpl::estimateNDVMultiKey(
    const std::vector<FieldPathAndEqSemantics>& fields,
    boost::optional<std::span<const OrderedIntervalList>> bounds) const {
    tassert(10061103, "Sample must be generated before calling estimateNDV()", _sample.has_value());

    if (!_spec.topLevelSampleFieldNames.empty()) {
        for (const auto& field : fields) {
            tassert(10061104,
                    "Sample must include the NDV fieldName as a top-level field.",
                    _spec.topLevelSampleFieldNames.contains(field.path.front()));
        }
    }

    // Obtain the NDV for the sample. If this is equal to the sample size, don't bother with NR
    // iteration, since it is likely to diverge. The best guess is that each element in the
    // collection is unique.
    const auto [totalSampleKeys, totalMatchingKeys, totalUniqueKeys] =
        countNDVMultiKey(fields, _sample->docs(), bounds);

    if (bounds.has_value() && !totalMatchingKeys) {
        return cost_based_ranker::zeroCE;
    }

    // NDV over an empty sample is undefined (there is no sensible default), and dividing by
    // a sample size of 0 below would yield NaN. This is a precondition violation: callers
    // must not request NDV when the sample is empty. (estimateNDV() enforces the analogous
    // invariant)
    tassert(
        12552502, "Multikey NDV estimation requires a non-empty sample", !_sample->docs().empty());
    const auto avgKeysPerDoc = (double(totalSampleKeys) / _sample->size());
    const auto estimatedIndexKeys = _collectionCard * avgKeysPerDoc;
    if (totalUniqueKeys == totalMatchingKeys) {
        LOGV2_DEBUG(10061105,
                    5,
                    "SamplingCE NDV is equal to the sample size, outputting estimated index size",
                    "fields"_attr = fields,
                    "sampleNDV"_attr = totalUniqueKeys,
                    "estimatedIndexKeys"_attr = estimatedIndexKeys);
        return estimatedIndexKeys;
    }

    CardinalityEstimate estimate = newtonRaphsonNDV(totalUniqueKeys, totalSampleKeys);
    LOGV2_DEBUG(10061106,
                5,
                "SamplingCE ndv (# unique values) for field",
                "fields"_attr = fields,
                "sampleNDV"_attr = totalUniqueKeys,
                "estimate"_attr = estimate);

    if (cost_based_ranker::exactGt(estimate, estimatedIndexKeys)) {
        LOGV2_DEBUG(10061107,
                    5,
                    "SamplingCE ndv exceeds estimated index size, rounding down",
                    "fields"_attr = fields,
                    "sampleNDV"_attr = totalUniqueKeys,
                    "estimate"_attr = estimate,
                    "estimatedIndexKeys"_attr = estimatedIndexKeys);
        estimate = estimatedIndexKeys;
    }
    return estimate;
}

std::unique_ptr<SamplingEstimator> SamplingEstimatorImpl::makeDefaultSamplingEstimator(
    const CanonicalQuery& cq,
    CardinalityEstimate collCard,
    PlanYieldPolicy::YieldPolicy yieldPolicy,
    const MultipleCollectionAccessor& collections,
    SamplingSourceEnum samplingSource) {
    const auto& qkc = cq.getExpCtx()->getQueryKnobConfiguration();
    const size_t sampleSize = calculateSampleSize(qkc);
    return std::unique_ptr<ce::SamplingEstimatorImpl>(
        new ce::SamplingEstimatorImpl(cq.getOpCtx(),
                                      collections,
                                      cq.nss(),
                                      yieldPolicy,
                                      sampleSize,
                                      qkc.getInternalQuerySamplingCEMethod(),
                                      qkc.getNumChunksForChunkBasedSampling(),
                                      collCard,
                                      cq.getExpCtx(),
                                      samplingSource,
                                      qkc.getInternalQuerySamplingCEMethodForPersistentSamples()));
}

}  // namespace mongo::ce
