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

#include "mongo/db/pipeline/document_source.h"

namespace mongo {

using boost::intrusive_ptr;
using std::pair;
using std::vector;

REGISTER_DOCUMENT_SOURCE(bucketAuto, DocumentSourceBucketAuto::createFromBson);

const char* DocumentSourceBucketAuto::getSourceName() const {
    return "$bucketAuto";
}

boost::optional<Document> DocumentSourceBucketAuto::getNext() {
    pExpCtx->checkForInterrupt();

    if (!_populated) {
        populateSorter();
        populateBuckets();

        _populated = true;
        _bucketsIterator = _buckets.begin();
    }

    if (_bucketsIterator == _buckets.end()) {
        dispose();
        return boost::none;
    }

    return makeDocument(*(_bucketsIterator++));
}

DocumentSource::GetDepsReturn DocumentSourceBucketAuto::getDependencies(DepsTracker* deps) const {
    // Add the 'groupBy' expression.
    _groupByExpression->addDependencies(deps);

    // Add the 'output' fields.
    for (auto&& exp : _expressions) {
        exp->addDependencies(deps);
    }

    // We know exactly which fields will be present in the output document. Future stages cannot
    // depend on any further fields. The grouping process will remove any metadata from the
    // documents, so there can be no further dependencies on metadata.
    return EXHAUSTIVE_ALL;
}

void DocumentSourceBucketAuto::populateSorter() {
    if (!_sorter) {
        SortOptions opts;
        opts.maxMemoryUsageBytes = _maxMemoryUsageBytes;
        const auto& valueCmp = pExpCtx->getValueComparator();
        auto comparator = [valueCmp](const Sorter<Value, Document>::Data& lhs,
                                     const Sorter<Value, Document>::Data& rhs) {
            return valueCmp.compare(lhs.first, rhs.first);
        };

        _sorter.reset(Sorter<Value, Document>::make(opts, comparator));
    }

    _nDocuments = 0;
    while (boost::optional<Document> next = pSource->getNext()) {
        Document doc = *next;
        _sorter->add(extractKey(doc), doc);
        _nDocuments++;
    }
}

Value DocumentSourceBucketAuto::extractKey(const Document& doc) {
    if (!_groupByExpression) {
        return Value(BSONNULL);
    }

    _variables->setRoot(doc);
    Value key = _groupByExpression->evaluate(_variables.get());

    // To be consistent with the $group stage, we consider "missing" to be equivalent to null when
    // grouping values into buckets.
    return key.missing() ? Value(BSONNULL) : std::move(key);
}

void DocumentSourceBucketAuto::addDocumentToBucket(const pair<Value, Document>& entry,
                                                   Bucket& bucket) {
    invariant(pExpCtx->getValueComparator().evaluate(entry.first >= bucket._max));
    bucket._max = entry.first;

    const size_t numAccumulators = _accumulatorFactories.size();
    _variables->setRoot(entry.second);
    for (size_t k = 0; k < numAccumulators; k++) {
        bucket._accums[k]->process(_expressions[k]->evaluate(_variables.get()), false);
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
        Bucket currentBucket(currentValue.first, currentValue.first, _accumulatorFactories);

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

            // If there are any more values that are equal to the boundary value, then absorb them
            // into the current bucket too.
            while (nextValue &&
                   pExpCtx->getValueComparator().evaluate(currentBucket._max == nextValue->first)) {
                addDocumentToBucket(*nextValue, currentBucket);
                nextValue = _sortedInput->more()
                    ? boost::optional<pair<Value, Document>>(_sortedInput->next())
                    : boost::none;
            }
            firstEntryInNextBucket = nextValue;
        }

        // Add the current bucket to the vector of buckets.
        addBucket(currentBucket);
    }
}

DocumentSourceBucketAuto::Bucket::Bucket(Value min,
                                         Value max,
                                         vector<Accumulator::Factory> accumulatorFactories)
    : _min(min), _max(max) {
    _accums.reserve(accumulatorFactories.size());
    for (auto&& factory : accumulatorFactories) {
        _accums.push_back(factory());
    }
}

void DocumentSourceBucketAuto::addBucket(const Bucket& newBucket) {
    // If there is a bucket that comes before the new bucket being added, then the previous bucket's
    // max boundary is updated to the new bucket's min. This is makes it so that buckets' min
    // boundaries are inclusive and max boundaries are exclusive (except for the last bucket, which
    // has an inclusive max).
    if (!_buckets.empty()) {
        Bucket& previous = _buckets.back();
        previous._max = newBucket._min;
    }
    _buckets.push_back(newBucket);
}

Document DocumentSourceBucketAuto::makeDocument(const Bucket& bucket) {
    const size_t nAccumulatedFields = _fieldNames.size();
    MutableDocument out(1 + nAccumulatedFields);

    out.addField("_id", Value{Document{{"min", bucket._min}, {"max", bucket._max}}});

    const bool mergingOutput = false;
    for (size_t i = 0; i < nAccumulatedFields; i++) {
        Value val = bucket._accums[i]->getValue(mergingOutput);

        // To be consistent with the $group stage, we consider "missing" to be equivalent to null
        // when evaluating accumulators.
        out.addField(_fieldNames[i], val.missing() ? Value(BSONNULL) : std::move(val));
    }
    return out.freeze();
}

void DocumentSourceBucketAuto::dispose() {
    _sortedInput.reset();
    _bucketsIterator = _buckets.end();
    pSource->dispose();
}

Value DocumentSourceBucketAuto::serialize(bool explain) const {
    MutableDocument insides;

    insides["groupBy"] = _groupByExpression->serialize(explain);
    insides["buckets"] = Value(_nBuckets);

    const size_t nOutputFields = _fieldNames.size();
    MutableDocument outputSpec(nOutputFields);
    for (size_t i = 0; i < nOutputFields; i++) {
        intrusive_ptr<Accumulator> accum = _accumulatorFactories[i]();
        outputSpec[_fieldNames[i]] =
            Value{Document{{accum->getOpName(), _expressions[i]->serialize(explain)}}};
    }
    insides["output"] = outputSpec.freezeToValue();

    // TODO SERVER-24152: handle granularity field

    return Value{Document{{getSourceName(), insides.freezeToValue()}}};
}

intrusive_ptr<DocumentSourceBucketAuto> DocumentSourceBucketAuto::create(
    const intrusive_ptr<ExpressionContext>& pExpCtx, int numBuckets, uint64_t maxMemoryUsageBytes) {
    return new DocumentSourceBucketAuto(pExpCtx, numBuckets, maxMemoryUsageBytes);
}

DocumentSourceBucketAuto::DocumentSourceBucketAuto(const intrusive_ptr<ExpressionContext>& pExpCtx,
                                                   int numBuckets,
                                                   uint64_t maxMemoryUsageBytes)
    : DocumentSource(pExpCtx), _nBuckets(numBuckets), _maxMemoryUsageBytes(maxMemoryUsageBytes) {}

void DocumentSourceBucketAuto::parseGroupByExpression(const BSONElement& groupByField,
                                                      const VariablesParseState& vps) {
    if (groupByField.type() == BSONType::Object &&
        groupByField.embeddedObject().firstElementFieldName()[0] == '$') {
        _groupByExpression = Expression::parseObject(groupByField.embeddedObject(), vps);
    } else if (groupByField.type() == BSONType::String &&
               groupByField.valueStringData()[0] == '$') {
        _groupByExpression = ExpressionFieldPath::parse(groupByField.str(), vps);
    } else {
        uasserted(
            40239,
            str::stream() << "The $bucketAuto 'groupBy' field must be defined as a $-prefixed "
                             "path or an expression object, but found: "
                          << groupByField.toString(false, false));
    }
}

void DocumentSourceBucketAuto::addAccumulator(StringData fieldName,
                                              Accumulator::Factory accumulatorFactory,
                                              const intrusive_ptr<Expression>& expression) {

    _fieldNames.push_back(fieldName.toString());
    _accumulatorFactories.push_back(accumulatorFactory);
    _expressions.push_back(expression);
}

intrusive_ptr<DocumentSource> DocumentSourceBucketAuto::createFromBson(
    BSONElement elem, const intrusive_ptr<ExpressionContext>& pExpCtx) {
    uassert(40240,
            str::stream() << "The argument to $bucketAuto must be an object, but found type: "
                          << typeName(elem.type()),
            elem.type() == BSONType::Object);

    intrusive_ptr<DocumentSourceBucketAuto> bucketAuto(DocumentSourceBucketAuto::create(pExpCtx));

    const BSONObj bucketAutoObj = elem.embeddedObject();
    VariablesIdGenerator idGenerator;
    VariablesParseState vps(&idGenerator);
    bool outputFieldSpecified = false;

    for (auto&& argument : bucketAutoObj) {
        const auto argName = argument.fieldNameStringData();
        if ("groupBy" == argName) {
            bucketAuto->parseGroupByExpression(argument, vps);
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

            int numBuckets = bucketsValue.coerceToInt();
            uassert(40243,
                    str::stream()
                        << "The $bucketAuto 'buckets' field must be greater than 0, but found: "
                        << numBuckets,
                    numBuckets > 0);
            bucketAuto->_nBuckets = numBuckets;
        } else if ("output" == argName) {
            uassert(40244,
                    str::stream()
                        << "The $bucketAuto 'output' field must be an object, but found type: "
                        << typeName(argument.type()),
                    argument.type() == BSONType::Object);

            outputFieldSpecified = true;
            for (auto&& outputField : argument.embeddedObject()) {
                auto parsedAccumulator = Accumulator::parseAccumulator(outputField, vps);

                auto fieldName = parsedAccumulator.first;
                auto accExpression = parsedAccumulator.second;
                auto factory =
                    Accumulator::getFactory(outputField.embeddedObject().firstElementFieldName());

                bucketAuto->addAccumulator(fieldName, factory, accExpression);
            }
        } else {
            uasserted(40245, str::stream() << "Unrecognized option to $bucketAuto: " << argName);
        }

        // TODO SERVER-24152: handle granularity field
    }

    uassert(40246,
            "$bucketAuto requires 'groupBy' and 'buckets' to be specified",
            bucketAuto->_groupByExpression && bucketAuto->_nBuckets > 0);

    // If there is no output field specified, then add the default one.
    if (!outputFieldSpecified) {
        bucketAuto->addAccumulator("count"_sd,
                                   Accumulator::getFactory("$sum"),
                                   ExpressionConstant::create(pExpCtx, Value(1)));
    }

    bucketAuto->_variables.reset(new Variables(idGenerator.getIdCount()));

    return bucketAuto;
}
}  // namespace mongo

#include "mongo/db/sorter/sorter.cpp"
// Explicit instantiation unneeded since we aren't exposing Sorter outside of this file.
