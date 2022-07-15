/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/pipeline/document_source_bucket_auto.h"

#include "mongo/db/pipeline/accumulation_statement.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/stats/resource_consumption_metrics.h"

namespace mongo {

using boost::intrusive_ptr;
using std::pair;
using std::string;
using std::vector;

REGISTER_DOCUMENT_SOURCE(bucketAuto,
                         LiteParsedDocumentSourceDefault::parse,
                         DocumentSourceBucketAuto::createFromBson,
                         AllowedWithApiStrict::kAlways);

namespace {

boost::intrusive_ptr<Expression> parseGroupByExpression(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const BSONElement& groupByField,
    const VariablesParseState& vps) {
    if (groupByField.type() == BSONType::Object &&
        groupByField.embeddedObject().firstElementFieldName()[0] == '$') {
        return Expression::parseObject(expCtx.get(), groupByField.embeddedObject(), vps);
    } else if (groupByField.type() == BSONType::String &&
               groupByField.valueStringData()[0] == '$') {
        return ExpressionFieldPath::parse(expCtx.get(), groupByField.str(), vps);
    } else {
        uasserted(
            40239,
            str::stream() << "The $bucketAuto 'groupBy' field must be defined as a $-prefixed "
                             "path or an expression object, but found: "
                          << groupByField.toString(false, false));
    }
}

/**
 * Generates a new file name on each call using a static, atomic and monotonically increasing
 * number.
 *
 * Each user of the Sorter must implement this function to ensure that all temporary files that the
 * Sorter instances produce are uniquely identified using a unique file name extension with separate
 * atomic variable. This is necessary because the sorter.cpp code is separately included in multiple
 * places, rather than compiled in one place and linked, and so cannot provide a globally unique ID.
 */
std::string nextFileName() {
    static AtomicWord<unsigned> documentSourceBucketAutoFileCounter;
    return "extsort-doc-bucket." +
        std::to_string(documentSourceBucketAutoFileCounter.fetchAndAdd(1));
}

}  // namespace

const char* DocumentSourceBucketAuto::getSourceName() const {
    return kStageName.rawData();
}

DocumentSource::GetNextResult DocumentSourceBucketAuto::doGetNext() {
    if (!_populated) {
        const auto populationResult = populateSorter();
        if (populationResult.isPaused()) {
            return populationResult;
        }
        invariant(populationResult.isEOF());

        initializeBucketIteration();
        _populated = true;
    }

    if (!_sortedInput) {
        // We have been disposed. Return EOF.
        return GetNextResult::makeEOF();
    }

    if (_currentBucketDetails.currentBucketNum++ < _nBuckets) {
        if (auto bucket = populateNextBucket()) {
            return makeDocument(*bucket);
        }
    }
    dispose();
    return GetNextResult::makeEOF();
}

boost::intrusive_ptr<DocumentSource> DocumentSourceBucketAuto::optimize() {
    _groupByExpression = _groupByExpression->optimize();
    for (auto&& accumulatedField : _accumulatedFields) {
        accumulatedField.expr.argument = accumulatedField.expr.argument->optimize();
        accumulatedField.expr.initializer = accumulatedField.expr.initializer->optimize();
    }
    return this;
}

DepsTracker::State DocumentSourceBucketAuto::getDependencies(DepsTracker* deps) const {
    // Add the 'groupBy' expression.
    _groupByExpression->addDependencies(deps);

    // Add the 'output' fields.
    for (auto&& accumulatedField : _accumulatedFields) {
        // Anything the per-doc expression depends on, the whole stage depends on.
        accumulatedField.expr.argument->addDependencies(deps);
        // The initializer should be an ExpressionConstant, or something that optimizes to one.
        // ExpressionConstant doesn't have dependencies.
    }

    // We know exactly which fields will be present in the output document. Future stages cannot
    // depend on any further fields. The grouping process will remove any metadata from the
    // documents, so there can be no further dependencies on metadata.
    return DepsTracker::State::EXHAUSTIVE_ALL;
}

DocumentSource::GetNextResult DocumentSourceBucketAuto::populateSorter() {
    if (!_sorter) {
        SortOptions opts;
        opts.maxMemoryUsageBytes = _maxMemoryUsageBytes;
        if (pExpCtx->allowDiskUse && !pExpCtx->inMongos) {
            opts.extSortAllowed = true;
            opts.tempDir = pExpCtx->tempDir;
        }
        const auto& valueCmp = pExpCtx->getValueComparator();
        auto comparator = [valueCmp](const Sorter<Value, Document>::Data& lhs,
                                     const Sorter<Value, Document>::Data& rhs) {
            return valueCmp.compare(lhs.first, rhs.first);
        };

        _sorter.reset(Sorter<Value, Document>::make(opts, comparator));
    }

    auto next = pSource->getNext();
    for (; next.isAdvanced(); next = pSource->getNext()) {
        auto nextDoc = next.releaseDocument();
        _sorter->add(extractKey(nextDoc), nextDoc);
        ++_nDocuments;
    }
    return next;
}

Value DocumentSourceBucketAuto::extractKey(const Document& doc) {
    if (!_groupByExpression) {
        return Value(BSONNULL);
    }

    Value key = _groupByExpression->evaluate(doc, &pExpCtx->variables);

    if (_granularityRounder) {
        uassert(40258,
                str::stream() << "$bucketAuto can specify a 'granularity' with numeric boundaries "
                                 "only, but found a value with type: "
                              << typeName(key.getType()),
                key.numeric());

        double keyValue = key.coerceToDouble();
        uassert(
            40259,
            "$bucketAuto can specify a 'granularity' with numeric boundaries only, but found a NaN",
            !std::isnan(keyValue));

        uassert(40260,
                "$bucketAuto can specify a 'granularity' with non-negative numbers only, but found "
                "a negative number",
                keyValue >= 0.0);
    }

    // To be consistent with the $group stage, we consider "missing" to be equivalent to null when
    // grouping values into buckets.
    return key.missing() ? Value(BSONNULL) : std::move(key);
}

void DocumentSourceBucketAuto::addDocumentToBucket(const pair<Value, Document>& entry,
                                                   Bucket& bucket) {
    invariant(pExpCtx->getValueComparator().evaluate(entry.first >= bucket._max));
    bucket._max = entry.first;

    const size_t numAccumulators = _accumulatedFields.size();
    for (size_t k = 0; k < numAccumulators; k++) {
        if (bucket._accums[k]->needsInput()) {
            bucket._accums[k]->process(
                _accumulatedFields[k].expr.argument->evaluate(entry.second, &pExpCtx->variables),
                false);
        }
    }
}

void DocumentSourceBucketAuto::initializeBucketIteration() {
    // Initialize the iterator on '_sorter'.
    invariant(_sorter);
    _sortedInput.reset(_sorter->done());

    auto& metricsCollector = ResourceConsumption::MetricsCollector::get(pExpCtx->opCtx);
    metricsCollector.incrementKeysSorted(_sorter->numSorted());
    metricsCollector.incrementSorterSpills(_sorter->stats().spilledRanges());

    _sorter.reset();

    // If there are no buckets, then we don't need to populate anything.
    if (_nBuckets == 0) {
        return;
    }

    // Calculate the approximate bucket size. We attempt to fill each bucket with this many
    // documents.
    _currentBucketDetails.approxBucketSize = std::round(double(_nDocuments) / double(_nBuckets));

    if (_currentBucketDetails.approxBucketSize < 1) {
        // If the number of buckets is larger than the number of documents, then we try to make
        // as many buckets as possible by placing each document in its own bucket.
        _currentBucketDetails.approxBucketSize = 1;
    }
}

boost::optional<pair<Value, Document>>
DocumentSourceBucketAuto::adjustBoundariesAndGetMinForNextBucket(Bucket* currentBucket) {
    auto getNextValIfPresent = [this]() {
        return _sortedInput->more() ? boost::optional<pair<Value, Document>>(_sortedInput->next())
                                    : boost::none;
    };

    auto nextValue = getNextValIfPresent();
    if (_granularityRounder) {
        Value boundaryValue = _granularityRounder->roundUp(currentBucket->_max);

        // If there are any values that now fall into this bucket after we round the
        // boundary, absorb them into this bucket too.
        while (nextValue &&
               pExpCtx->getValueComparator().evaluate(boundaryValue > nextValue->first)) {
            addDocumentToBucket(*nextValue, *currentBucket);
            nextValue = getNextValIfPresent();
        }

        // Handle the special case where the largest value in the first bucket is zero. In this
        // case, we take the minimum boundary of the next bucket and round it down. We then set the
        // maximum boundary of the current bucket to be the rounded down value. This maintains that
        // the maximum boundary of the current bucket is exclusive and the minimum boundary of the
        // next bucket is inclusive.
        double currentMax = boundaryValue.coerceToDouble();
        if (currentMax == 0.0 && nextValue) {
            currentBucket->_max = _granularityRounder->roundDown(nextValue->first);
        } else {
            currentBucket->_max = boundaryValue;
        }
    } else {
        // If there are any more values that are equal to the boundary value, then absorb
        // them into the current bucket too.
        while (nextValue &&
               pExpCtx->getValueComparator().evaluate(currentBucket->_max == nextValue->first)) {
            addDocumentToBucket(*nextValue, *currentBucket);
            nextValue = getNextValIfPresent();
        }

        // If there is a bucket that comes after the current bucket, then the current bucket's max
        // boundary is updated to the next bucket's min. This makes it so that buckets' min
        // boundaries are inclusive and max boundaries are exclusive (except for the last bucket,
        // which has an inclusive max).
        if (nextValue) {
            currentBucket->_max = nextValue->first;
        }
    }
    return nextValue;
}

boost::optional<DocumentSourceBucketAuto::Bucket> DocumentSourceBucketAuto::populateNextBucket() {
    // If there was a bucket before this, the 'currentMin' should be populated, or there are no more
    // documents.
    if (!_currentBucketDetails.currentMin && !_sortedInput->more()) {
        return {};
    }

    std::pair<Value, Document> currentValue =
        _currentBucketDetails.currentMin ? *_currentBucketDetails.currentMin : _sortedInput->next();

    Bucket currentBucket(pExpCtx, currentValue.first, currentValue.first, _accumulatedFields);

    // If we have a granularity specified and if there is a bucket that came before the current
    // bucket being added, then the current bucket's min boundary is updated to be the previous
    // bucket's max boundary. This makes it so that bucket boundaries follow the granularity, have
    // inclusive minimums, and have exclusive maximums.
    if (_granularityRounder) {
        currentBucket._min = _currentBucketDetails.previousMax.value_or(
            _granularityRounder->roundDown(currentValue.first));
    }

    // Evaluate each initializer against an empty document. Normally the initializer can refer to
    // the group key, but in $bucketAuto there is no single group key per bucket.
    Document emptyDoc;
    for (size_t k = 0; k < _accumulatedFields.size(); ++k) {
        Value initializerValue =
            _accumulatedFields[k].expr.initializer->evaluate(emptyDoc, &pExpCtx->variables);
        currentBucket._accums[k]->startNewGroup(initializerValue);
    }


    // Add 'approxBucketSize' number of documents to the current bucket. If this is the last bucket,
    // add all the remaining documents.
    addDocumentToBucket(currentValue, currentBucket);
    const auto isLastBucket = (_currentBucketDetails.currentBucketNum == _nBuckets);
    for (long long i = 1;
         _sortedInput->more() && (i < _currentBucketDetails.approxBucketSize || isLastBucket);
         i++) {
        addDocumentToBucket(_sortedInput->next(), currentBucket);
    }

    // Modify the bucket details for next bucket.
    _currentBucketDetails.currentMin = adjustBoundariesAndGetMinForNextBucket(&currentBucket);
    _currentBucketDetails.previousMax = currentBucket._max;
    return currentBucket;
}

DocumentSourceBucketAuto::Bucket::Bucket(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    Value min,
    Value max,
    const vector<AccumulationStatement>& accumulationStatements)
    : _min(min), _max(max) {
    _accums.reserve(accumulationStatements.size());
    for (auto&& accumulationStatement : accumulationStatements) {
        _accums.push_back(accumulationStatement.makeAccumulator());
    }
}

Document DocumentSourceBucketAuto::makeDocument(const Bucket& bucket) {
    const size_t nAccumulatedFields = _accumulatedFields.size();
    MutableDocument out(1 + nAccumulatedFields);

    out.addField("_id", Value{Document{{"min", bucket._min}, {"max", bucket._max}}});

    const bool mergingOutput = false;
    for (size_t i = 0; i < nAccumulatedFields; i++) {
        Value val = bucket._accums[i]->getValue(mergingOutput);

        // To be consistent with the $group stage, we consider "missing" to be equivalent to null
        // when evaluating accumulators.
        out.addField(_accumulatedFields[i].fieldName,
                     val.missing() ? Value(BSONNULL) : std::move(val));
    }
    return out.freeze();
}

void DocumentSourceBucketAuto::doDispose() {
    _sortedInput.reset();
}

Value DocumentSourceBucketAuto::serialize(
    boost::optional<ExplainOptions::Verbosity> explain) const {
    MutableDocument insides;

    insides["groupBy"] = _groupByExpression->serialize(static_cast<bool>(explain));
    insides["buckets"] = Value(_nBuckets);

    if (_granularityRounder) {
        insides["granularity"] = Value(_granularityRounder->getName());
    }

    MutableDocument outputSpec(_accumulatedFields.size());
    for (auto&& accumulatedField : _accumulatedFields) {
        intrusive_ptr<AccumulatorState> accum = accumulatedField.makeAccumulator();
        outputSpec[accumulatedField.fieldName] =
            Value(accum->serialize(accumulatedField.expr.initializer,
                                   accumulatedField.expr.argument,
                                   static_cast<bool>(explain)));
    }
    insides["output"] = outputSpec.freezeToValue();

    return Value{Document{{getSourceName(), insides.freezeToValue()}}};
}

intrusive_ptr<DocumentSourceBucketAuto> DocumentSourceBucketAuto::create(
    const intrusive_ptr<ExpressionContext>& pExpCtx,
    const boost::intrusive_ptr<Expression>& groupByExpression,
    int numBuckets,
    std::vector<AccumulationStatement> accumulationStatements,
    const boost::intrusive_ptr<GranularityRounder>& granularityRounder,
    uint64_t maxMemoryUsageBytes) {
    uassert(40243,
            str::stream() << "The $bucketAuto 'buckets' field must be greater than 0, but found: "
                          << numBuckets,
            numBuckets > 0);
    // If there is no output field specified, then add the default one.
    if (accumulationStatements.empty()) {
        accumulationStatements.emplace_back(
            "count",
            AccumulationExpression(ExpressionConstant::create(pExpCtx.get(), Value(BSONNULL)),
                                   ExpressionConstant::create(pExpCtx.get(), Value(1)),
                                   [pExpCtx] { return AccumulatorSum::create(pExpCtx.get()); },
                                   AccumulatorSum::kName));
    }
    return new DocumentSourceBucketAuto(pExpCtx,
                                        groupByExpression,
                                        numBuckets,
                                        accumulationStatements,
                                        granularityRounder,
                                        maxMemoryUsageBytes);
}

DocumentSourceBucketAuto::DocumentSourceBucketAuto(
    const intrusive_ptr<ExpressionContext>& pExpCtx,
    const boost::intrusive_ptr<Expression>& groupByExpression,
    int numBuckets,
    std::vector<AccumulationStatement> accumulationStatements,
    const boost::intrusive_ptr<GranularityRounder>& granularityRounder,
    uint64_t maxMemoryUsageBytes)
    : DocumentSource(kStageName, pExpCtx),
      _maxMemoryUsageBytes(maxMemoryUsageBytes),
      _groupByExpression(groupByExpression),
      _granularityRounder(granularityRounder),
      _nBuckets(numBuckets),
      _currentBucketDetails{0} {
    invariant(!accumulationStatements.empty());
    for (auto&& accumulationStatement : accumulationStatements) {
        _accumulatedFields.push_back(accumulationStatement);
    }
}

boost::intrusive_ptr<Expression> DocumentSourceBucketAuto::getGroupByExpression() const {
    return _groupByExpression;
}

const std::vector<AccumulationStatement>& DocumentSourceBucketAuto::getAccumulatedFields() const {
    return _accumulatedFields;
}

intrusive_ptr<DocumentSource> DocumentSourceBucketAuto::createFromBson(
    BSONElement elem, const intrusive_ptr<ExpressionContext>& pExpCtx) {
    uassert(40240,
            str::stream() << "The argument to $bucketAuto must be an object, but found type: "
                          << typeName(elem.type()),
            elem.type() == BSONType::Object);

    VariablesParseState vps = pExpCtx->variablesParseState;
    vector<AccumulationStatement> accumulationStatements;
    boost::intrusive_ptr<Expression> groupByExpression;
    boost::optional<int> numBuckets;
    boost::intrusive_ptr<GranularityRounder> granularityRounder;

    pExpCtx->sbeCompatible = false;
    for (auto&& argument : elem.Obj()) {
        const auto argName = argument.fieldNameStringData();
        if ("groupBy" == argName) {
            groupByExpression = parseGroupByExpression(pExpCtx, argument, vps);
        } else if ("buckets" == argName) {
            Value bucketsValue = Value(argument);

            uassert(
                40241,
                str::stream()
                    << "The $bucketAuto 'buckets' field must be a numeric value, but found type: "
                    << typeName(argument.type()),
                bucketsValue.numeric());

            uassert(40242,
                    str::stream() << "The $bucketAuto 'buckets' field must be representable as a "
                                     "32-bit integer, but found "
                                  << Value(argument).coerceToDouble(),
                    bucketsValue.integral());

            numBuckets = bucketsValue.coerceToInt();
        } else if ("output" == argName) {
            uassert(40244,
                    str::stream()
                        << "The $bucketAuto 'output' field must be an object, but found type: "
                        << typeName(argument.type()),
                    argument.type() == BSONType::Object);

            for (auto&& outputField : argument.embeddedObject()) {
                auto stmt = AccumulationStatement::parseAccumulationStatement(
                    pExpCtx.get(), outputField, vps);
                stmt.expr.initializer = stmt.expr.initializer->optimize();
                uassert(4544714,
                        "Can't refer to the group key in $bucketAuto",
                        ExpressionConstant::isNullOrConstant(stmt.expr.initializer));
                accumulationStatements.push_back(std::move(stmt));
            }
        } else if ("granularity" == argName) {
            uassert(40261,
                    str::stream()
                        << "The $bucketAuto 'granularity' field must be a string, but found type: "
                        << typeName(argument.type()),
                    argument.type() == BSONType::String);
            granularityRounder = GranularityRounder::getGranularityRounder(pExpCtx, argument.str());
        } else {
            uasserted(40245, str::stream() << "Unrecognized option to $bucketAuto: " << argName);
        }
    }

    uassert(40246,
            "$bucketAuto requires 'groupBy' and 'buckets' to be specified",
            groupByExpression && numBuckets);

    return DocumentSourceBucketAuto::create(
        pExpCtx, groupByExpression, numBuckets.get(), accumulationStatements, granularityRounder);
}

}  // namespace mongo

#include "mongo/db/sorter/sorter.cpp"
// Explicit instantiation unneeded since we aren't exposing Sorter outside of this file.
