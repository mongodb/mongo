/**
 *    Copyright (C) 2016 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
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

namespace mongo {

using boost::intrusive_ptr;
using std::pair;
using std::string;
using std::vector;

REGISTER_DOCUMENT_SOURCE(bucketAuto,
                         LiteParsedDocumentSourceDefault::parse,
                         DocumentSourceBucketAuto::createFromBson);

const char* DocumentSourceBucketAuto::getSourceName() const {
    return "$bucketAuto";
}

DocumentSource::GetNextResult DocumentSourceBucketAuto::getNext() {
    pExpCtx->checkForInterrupt();

    if (!_populated) {
        const auto populationResult = populateSorter();
        if (populationResult.isPaused()) {
            return populationResult;
        }
        invariant(populationResult.isEOF());

        populateBuckets();

        _populated = true;
        _bucketsIterator = _buckets.begin();
    }

    if (_bucketsIterator == _buckets.end()) {
        dispose();
        return GetNextResult::makeEOF();
    }

    return makeDocument(*(_bucketsIterator++));
}

DocumentSource::GetDepsReturn DocumentSourceBucketAuto::getDependencies(DepsTracker* deps) const {
    // Add the 'groupBy' expression.
    _groupByExpression->addDependencies(deps);

    // Add the 'output' fields.
    for (auto&& accumulatedField : _accumulatedFields) {
        accumulatedField.expression->addDependencies(deps);
    }

    // We know exactly which fields will be present in the output document. Future stages cannot
    // depend on any further fields. The grouping process will remove any metadata from the
    // documents, so there can be no further dependencies on metadata.
    return EXHAUSTIVE_ALL;
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
        _nDocuments++;
    }
    return next;
}

Value DocumentSourceBucketAuto::extractKey(const Document& doc) {
    if (!_groupByExpression) {
        return Value(BSONNULL);
    }

    Value key = _groupByExpression->evaluate(doc);

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
        bucket._accums[k]->process(_accumulatedFields[k].expression->evaluate(entry.second), false);
    }
}

void DocumentSourceBucketAuto::populateBuckets() {
    invariant(_sorter);
    _sortedInput.reset(_sorter->done());
    _sorter.reset();

    // If there are no buckets, then we don't need to populate anything.
    if (_nBuckets == 0) {
        return;
    }

    // Calculate the approximate bucket size. We attempt to fill each bucket with this many
    // documents.
    long long approxBucketSize = std::round(double(_nDocuments) / double(_nBuckets));

    if (approxBucketSize < 1) {
        // If the number of buckets is larger than the number of documents, then we try to make as
        // many buckets as possible by placing each document in its own bucket.
        approxBucketSize = 1;
    }

    boost::optional<pair<Value, Document>> firstEntryInNextBucket;

    // Start creating and populating the buckets.
    for (int i = 0; i < _nBuckets; i++) {
        bool isLastBucket = (i == _nBuckets - 1);

        // Get the first value to place in this bucket.
        pair<Value, Document> currentValue;
        if (firstEntryInNextBucket) {
            currentValue = *firstEntryInNextBucket;
            firstEntryInNextBucket = boost::none;
        } else if (_sortedInput->more()) {
            currentValue = _sortedInput->next();
        } else {
            // No more values to process.
            break;
        }

        // Initialize the current bucket.
        Bucket currentBucket(pExpCtx, currentValue.first, currentValue.first, _accumulatedFields);

        // Add the first value into the current bucket.
        addDocumentToBucket(currentValue, currentBucket);

        if (isLastBucket) {
            // If this is the last bucket allowed, we need to put any remaining documents in
            // the current bucket.
            while (_sortedInput->more()) {
                addDocumentToBucket(_sortedInput->next(), currentBucket);
            }
        } else {
            // We go to approxBucketSize - 1 because we already added the first value in order
            // to keep track of the minimum value.
            for (long long j = 0; j < approxBucketSize - 1; j++) {
                if (_sortedInput->more()) {
                    addDocumentToBucket(_sortedInput->next(), currentBucket);
                } else {
                    // No more values to process.
                    break;
                }
            }

            boost::optional<pair<Value, Document>> nextValue = _sortedInput->more()
                ? boost::optional<pair<Value, Document>>(_sortedInput->next())
                : boost::none;

            if (_granularityRounder) {
                Value boundaryValue = _granularityRounder->roundUp(currentBucket._max);
                // If there are any values that now fall into this bucket after we round the
                // boundary, absorb them into this bucket too.
                while (nextValue &&
                       pExpCtx->getValueComparator().evaluate(boundaryValue > nextValue->first)) {
                    addDocumentToBucket(*nextValue, currentBucket);
                    nextValue = _sortedInput->more()
                        ? boost::optional<pair<Value, Document>>(_sortedInput->next())
                        : boost::none;
                }
                if (nextValue) {
                    currentBucket._max = boundaryValue;
                }
            } else {
                // If there are any more values that are equal to the boundary value, then absorb
                // them into the current bucket too.
                while (nextValue &&
                       pExpCtx->getValueComparator().evaluate(currentBucket._max ==
                                                              nextValue->first)) {
                    addDocumentToBucket(*nextValue, currentBucket);
                    nextValue = _sortedInput->more()
                        ? boost::optional<pair<Value, Document>>(_sortedInput->next())
                        : boost::none;
                }
            }
            firstEntryInNextBucket = nextValue;
        }

        // Add the current bucket to the vector of buckets.
        addBucket(currentBucket);
    }

    if (!_buckets.empty() && _granularityRounder) {
        // If we we have a granularity, we round the first bucket's minimum down and the last
        // bucket's maximum up. This way all of the bucket boundaries are rounded to numbers in the
        // granularity specification.
        Bucket& firstBucket = _buckets.front();
        Bucket& lastBucket = _buckets.back();
        firstBucket._min = _granularityRounder->roundDown(firstBucket._min);
        lastBucket._max = _granularityRounder->roundUp(lastBucket._max);
    }
}

DocumentSourceBucketAuto::Bucket::Bucket(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    Value min,
    Value max,
    const vector<AccumulationStatement>& accumulationStatements)
    : _min(min), _max(max) {
    _accums.reserve(accumulationStatements.size());
    for (auto&& accumulationStatement : accumulationStatements) {
        _accums.push_back(accumulationStatement.makeAccumulator(expCtx));
    }
}

void DocumentSourceBucketAuto::addBucket(Bucket& newBucket) {
    if (!_buckets.empty()) {
        Bucket& previous = _buckets.back();
        if (_granularityRounder) {
            // If we have a granularity specified and if there is a bucket that comes before the new
            // bucket being added, then the new bucket's min boundary is updated to be the
            // previous bucket's max boundary. This makes it so that bucket boundaries follow the
            // granularity, have inclusive minimums, and have exclusive maximums.

            double prevMax = previous._max.coerceToDouble();
            if (prevMax == 0.0) {
                // Handle the special case where the largest value in the first bucket is zero. In
                // this case, we take the minimum boundary of the second bucket and round it down.
                // We then set the maximum boundary of the first bucket to be the rounded down
                // value. This maintains that the maximum boundary of the first bucket is exclusive
                // and the minimum boundary of the second bucket is inclusive.
                previous._max = _granularityRounder->roundDown(newBucket._min);
            }

            newBucket._min = previous._max;
        } else {
            // If there is a bucket that comes before the new bucket being added, then the previous
            // bucket's max boundary is updated to the new bucket's min. This makes it so that
            // buckets' min boundaries are inclusive and max boundaries are exclusive (except for
            // the last bucket, which has an inclusive max).
            previous._max = newBucket._min;
        }
    }
    _buckets.push_back(newBucket);
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
    _bucketsIterator = _buckets.end();
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
        intrusive_ptr<Accumulator> accum = accumulatedField.makeAccumulator(pExpCtx);
        outputSpec[accumulatedField.fieldName] =
            Value{Document{{accum->getOpName(),
                            accumulatedField.expression->serialize(static_cast<bool>(explain))}}};
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
        accumulationStatements.emplace_back("count",
                                            ExpressionConstant::create(pExpCtx, Value(1)),
                                            AccumulationStatement::getFactory("$sum"));
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
    : DocumentSource(pExpCtx),
      _nBuckets(numBuckets),
      _maxMemoryUsageBytes(maxMemoryUsageBytes),
      _groupByExpression(groupByExpression),
      _granularityRounder(granularityRounder) {

    invariant(!accumulationStatements.empty());
    for (auto&& accumulationStatement : accumulationStatements) {
        _accumulatedFields.push_back(accumulationStatement);
    }
}

namespace {

boost::intrusive_ptr<Expression> parseGroupByExpression(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const BSONElement& groupByField,
    const VariablesParseState& vps) {
    if (groupByField.type() == BSONType::Object &&
        groupByField.embeddedObject().firstElementFieldName()[0] == '$') {
        return Expression::parseObject(expCtx, groupByField.embeddedObject(), vps);
    } else if (groupByField.type() == BSONType::String &&
               groupByField.valueStringData()[0] == '$') {
        return ExpressionFieldPath::parse(expCtx, groupByField.str(), vps);
    } else {
        uasserted(
            40239,
            str::stream() << "The $bucketAuto 'groupBy' field must be defined as a $-prefixed "
                             "path or an expression object, but found: "
                          << groupByField.toString(false, false));
    }
}
}  // namespace

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
                accumulationStatements.push_back(
                    AccumulationStatement::parseAccumulationStatement(pExpCtx, outputField, vps));
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
