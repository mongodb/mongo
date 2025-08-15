/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/query/compiler/ce/ce_common.h"
#include "mongo/db/query/compiler/stats/rand_utils_new.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo::ce {
// Enable this flag to log all estimates, and let all tests pass.
constexpr bool kCETestLogOnly = false;
const double kMaxCEError = 0.01;
constexpr double kErrorBound = 0.01;
static constexpr size_t kPredefinedArraySize = 15;

enum QueryType { kPoint, kRange };

/**
 * This struct represents the building block for type definition for generating values with varying
 * data types with specific probability. The typeProbability represents the probability [0,100] with
 * which typeTag will appear in the resulting dataset. TypeProbability is used as part of
 * TypeCombination, the total probability in all types in TypeCombination must be 100.
 */
struct TypeProbability {
    friend std::ostream& operator<<(std::ostream& os, const TypeProbability& tp) {
        os << tp.typeTag << '/' << tp.typeProbability << '/' << tp.nanProb;
        return os;
    }

    sbe::value::TypeTags typeTag;

    // Type probability [0,100]
    size_t typeProbability;

    // Probability of NaN Value [0,1]
    double nanProb = 0.0;
};

using TypeTags = sbe::value::TypeTags;
using TypeCombination = std::vector<TypeProbability>;
using TypeCombinations = std::vector<TypeCombination>;

struct QueryInfoAndResults {
    boost::optional<stats::SBEValue> low;
    boost::optional<stats::SBEValue> high;
    boost::optional<std::string> matchExpression;
    size_t actualCardinality;
    double estimatedCardinality;
};

struct ErrorCalculationSummary {
    // query information and results.
    std::vector<QueryInfoAndResults> queryResults;

    // total executed queries.
    size_t executedQueries = 0;
};

TypeProbability parseCollectionType(sbe::value::TypeTags dataType, bool nan = false);

std::pair<size_t, size_t> parseDataTypeToInterval(sbe::value::TypeTags dataType);

/**
 * This struct stores the data to generate a specific collection field including field names, type,
 * and statistics input for generating values.
 * This struct is used for both the generation of data for populating a collection as well as
 * generating data for queries.
 */
struct DataFieldDefinition {
    /**
     * Constructor for query field definition allowing for defining only one seed.
     */
    DataFieldDefinition(std::string fieldName,
                        sbe::value::TypeTags fieldDataType,
                        int ndv,
                        stats::DistrType dataDistribution,
                        size_t seed,
                        double nanProb = 0,
                        size_t arrayTypeLength = 0)
        : fieldName(fieldName),
          dataDistribution(dataDistribution),
          typeCombinationData({parseCollectionType(fieldDataType)}),
          ndv(ndv),
          seed1(seed),
          seed2(seed),
          nanProb(nanProb),
          arrayTypeLength(arrayTypeLength) {
        dataInterval = parseDataTypeToInterval(fieldDataType);
    }

    /**
     * Constructor for query field definition allowing for defining seeds for high and low values.
     */
    DataFieldDefinition(std::string fieldName,
                        sbe::value::TypeTags fieldDataType,
                        int ndv,
                        stats::DistrType dataDistribution,
                        std::pair<size_t, size_t> seed,
                        double nanProb = 0,
                        size_t arrayTypeLength = 0)
        : fieldName(fieldName),
          dataDistribution(dataDistribution),
          typeCombinationData({parseCollectionType(fieldDataType)}),
          ndv(ndv),
          seed1(seed.first),
          seed2(seed.second),
          nanProb(nanProb),
          arrayTypeLength(arrayTypeLength) {
        dataInterval = parseDataTypeToInterval(fieldDataType);
    }

    void addToBSONObjBuilder(BSONObjBuilder& builder) const;

    // The name of this field.
    std::string fieldName;

    // Define the data distribution this field follows.
    stats::DistrType dataDistribution;

    // Define the data types that this field contains.
    TypeCombination typeCombinationData;

    // Define the number of distinct values of this field.
    int ndv;

    // The data generation seed.
    size_t seed1;
    // We allow two seeds to keep metadata for query interval generation.
    size_t seed2;

    // Inclusive minimum and maximum bounds for randomly generated data, ensuring each data
    // falls within these limits.
    std::pair<size_t, size_t> dataInterval;

    // Probability of NaN value appearing in a numeric type.
    double nanProb = 0;

    // The length of any value of Array type.
    size_t arrayTypeLength = 0;
};

/**
 * This struct defines the configuration for generating a specific document field including the
 * location of that field in the collection.
 * This class is used for generating data for collections.
 */
struct CollectionFieldConfiguration : public DataFieldDefinition {
    // Zero-based position of this field in the Collection.
    int fieldPositionInCollection;

    CollectionFieldConfiguration(std::string fieldName,
                                 int fieldPositionInCollection,
                                 sbe::value::TypeTags fieldDataType,
                                 int ndv,
                                 stats::DistrType dataDistribution,
                                 size_t seed,
                                 double nanProb = 0,
                                 size_t arrayTypeLength = 0)
        : DataFieldDefinition(
              fieldName, fieldDataType, ndv, dataDistribution, seed, nanProb, arrayTypeLength),
          fieldPositionInCollection(fieldPositionInCollection) {}

    void addToBSONObjBuilder(BSONObjBuilder& builder) const;
};

/**
 * This struct defines the configuration for generating a collection with multiple fields.
 */
struct DataConfiguration {
    /**
     * Initialize DataConfiguration by providing individual datafield configurations.
     */
    DataConfiguration(size_t size, std::vector<CollectionFieldConfiguration> allDataFieldConfig)
        : size(size), collectionFieldsConfiguration(allDataFieldConfig) {}

    void addToBSONObjBuilder(BSONObjBuilder& builder) const;

    // The size of the collection to be generated.
    size_t size;

    // Vector defining specific fields along with their positions within the collection and the
    // configuration details for said field.
    std::vector<CollectionFieldConfiguration> collectionFieldsConfiguration;
};

/**
 * This struct defines the set of queries that will be executed.
 */
struct QueryConfiguration {
    QueryConfiguration() {}

    QueryConfiguration(QueryConfiguration& qc)
        : queryFields(qc.queryFields), queryTypes(qc.queryTypes) {}

    QueryConfiguration(QueryConfiguration&& qc)
        : queryFields(std::move(qc.queryFields)), queryTypes(std::move(qc.queryTypes)) {}

    QueryConfiguration(std::vector<DataFieldDefinition> newQueryFields,
                       std::vector<QueryType> queryTypes)
        : queryFields(newQueryFields), queryTypes(queryTypes) {
        tassert(10624001,
                "Number of query types and fields must match",
                newQueryFields.size() == queryTypes.size());
    };

    void addToBSONObjBuilder(BSONObjBuilder& builder) const;

    // Defines a vector of the fields that will be queried e.g., {a, b, c} along with all the
    // relevant metadata.
    std::vector<DataFieldDefinition> queryFields;

    // Defines the types of queries that will execute on the corresponding field. The length of
    // 'queryFields' and 'queryTypes' has to be identical.
    std::vector<QueryType> queryTypes;
};

/**
 * This class defines the queries that will be executed for a test, this struct allows for queries
 * over multiple fields defined in "queryFields".
 */
struct WorkloadConfiguration {
    WorkloadConfiguration() {}

    WorkloadConfiguration(WorkloadConfiguration& cp)
        : numberOfQueries(cp.numberOfQueries), queryConfig(cp.queryConfig) {};

    WorkloadConfiguration(WorkloadConfiguration&& cp)
        : numberOfQueries(cp.numberOfQueries), queryConfig(std::move(cp.queryConfig)) {};

    WorkloadConfiguration(size_t numberOfQueries, QueryConfiguration queryConfig)
        : numberOfQueries(numberOfQueries), queryConfig(queryConfig) {};

    void addToBSONObjBuilder(BSONObjBuilder& builder) const;

    // Define the number of queries to execute.
    size_t numberOfQueries;

    // Defines the query configuration.
    QueryConfiguration queryConfig;
};

size_t calculateCardinality(const MatchExpression* expr, std::vector<BSONObj> data);

/**
 * Populates TypeDistrVector 'td' based on the input configuration.
 *
 * This function iterates over a given type combination and populates the provided 'td' with
 * various statistical distributions according to the specified types and their probabilities.
 *
 * This function supports data types: nothing, null, boolean, integer, string, and array. Note
 * that currently, arrays are only generated with integer elements.
 *
 * @param td The TypeDistrVector that will be populated.
 * @param interval A pair representing the inclusive minimum and maximum bounds for the data.
 * @param typeCombination The types and their associated probabilities presenting the
 * distribution.
 * @param ndv The number of distinct values to generate.
 * @param seedArray A random number seed for generating array. Used only by TypeTags::Array.
 * @param mdd The distribution descriptor.
 * @param arrayLength The maximum length for array distributions, defaulting to 0.
 */
void populateTypeDistrVectorAccordingToInputConfig(stats::TypeDistrVector& td,
                                                   const std::pair<size_t, size_t>& interval,
                                                   const TypeCombination& typeCombination,
                                                   size_t ndv,
                                                   std::mt19937_64& seedArray,
                                                   stats::MixedDistributionDescriptor& mdd,
                                                   int arrayLength = 0);

/**
 * Populates a vector with values according to the metadata provided, the vector essentially
 * represents a data field spanning a multiple documents.
 *
 * @param ndv Number of distinct values
 * @param size The number of values to generate
 * @param typeCombination The types and their associated probabilities presenting the
 * distribution.
 * @param dataDistribution The data distribution to generate.
 * @param seedData A random number seed for generating the data..
 * @param arrayTypeLength The maximum length for array distributions.
 * @param data The resulting vector containing the generated values.
 */
void generateDataOneField(size_t ndv,
                          size_t size,
                          TypeCombination typeCombinationData,
                          stats::DistrType dataDistribution,
                          const std::pair<size_t, size_t>& dataInterval,
                          size_t seedData,
                          int arrayTypeLength,
                          std::vector<stats::SBEValue>& data);

/**
 * Populates a vector of vectors with values according to the metadata provided, the inner vectors
 * represents a data field spanning a multiple documents and the outer vector represents a vector of
 * fields.
 *
 * @param ndv Number of distinct values
 * @param size The number of values to generate
 * @param typeCombination The types and their associated probabilities presenting the
 * distribution.
 * @param dataDistribution The data distribution to generate.
 * @param seedData A random number seed for generating the data..
 * @param arrayTypeLength The maximum length for array distributions.
 * @param data The resulting vector containing the generated values.
 */
void generateDataBasedOnConfig(DataConfiguration& configuration,
                               std::vector<std::vector<stats::SBEValue>>& allData);


/**
 * Transform a vector of SBEValues to a vector BSONObj to allow the evaluation of
 * MatchExpression on the generated data. This function assumes that the input vector represents
 * a field in a collection (i.e., a column). The second argument corresponds to the name of that
 * field to add in the resulting BSONObjects.
 */
std::vector<BSONObj> transformSBEValueVectorToBSONObjVector(std::vector<stats::SBEValue> data,
                                                            std::string fieldName = "a");

/**
 * Transform a vector of vectors of SBEValues corresponding to a collection of documents to a vector
 * of BSONObjs to allow the evaluation of MatchExpression on the generated data. This function
 * assumes that the input vector represents a collection of documents (i.e., a vector of field
 * values). The second argument corresponds to the names of the fields to add in the resulting
 * BSONObjects.
 */
std::vector<BSONObj> transformSBEValueVectorOfVectorsToBSONObjVector(
    std::vector<std::vector<stats::SBEValue>> data, std::vector<std::string> fieldNames);

/**
 * Translate a simple query as defined by histogram CE accuracy and performance
 * benchmarks into a MatchExpression. This function assumes that the query is applied on a
 * specific field on a collection. If the queryType is point query only the sbeValLow is taken
 * into consideration. The last argument corresponds to the name of the field.
 * This function is relevant only for histogram CE and should be retired in favor of the vector
 * version.
 */
std::unique_ptr<MatchExpression> createQueryMatchExpression(QueryType queryType,
                                                            const stats::SBEValue& sbeValLow,
                                                            const stats::SBEValue& sbeValHigh,
                                                            StringData fieldName = "a");

/**
 * Translate a query over multiple fields as defined by histogram/sampling CE accuracy and
 * performance benchmarks into a MatchExpression. The size of the input vectors needs to be
 * identical and represents the number of predicates. The individual predicates are added into a
 * conjunction.
 */
std::unique_ptr<MatchExpression> createQueryMatchExpression(
    std::vector<QueryType> queryTypes,
    std::vector<std::pair<stats::SBEValue, stats::SBEValue>> queryFieldIntervals,
    std::vector<DataFieldDefinition> fieldNames);

/**
 * Create single MatchExpression based on the fields, queryTypes defined in the queryConfig, and the
 * interval values defiend in queryFieldsIntervals.
 * This function invokes 'createQueryMatchExpression' to generate individual MatchExpressions for
 * each field and creates a conjunction.
 */
std::vector<std::unique_ptr<MatchExpression>> createQueryMatchExpressionOnMultipleFields(
    WorkloadConfiguration queryConfig,
    std::vector<std::vector<std::pair<stats::SBEValue, stats::SBEValue>>> queryFieldsIntervals);

/**
 * A wrapper function invoking both generateMultiFieldIntervals and
 * createQueryMatchExpressionOnMultipleFields returning directly MatchExpressions on multiple
 * fields.
 */
std::vector<std::unique_ptr<MatchExpression>> generateMatchExpressionsBasedOnWorkloadConfig(
    WorkloadConfiguration& workloadConfig);

/**
 * Generates query intervals randomly according to testing configuration.
 * In case of point queries, this function still returns a pair containing the same value twice. In
 * this case, the consumer may disregard the second value.
 * For range queries, the function generates separately the lower and the higher values. The
 * function ensures lowerValue <= higherValue.
 *
 * @param queryType The type of query intervals. It can be either kPoint or kRange.
 * @param interval A pair representing the overall range [min, max] within which all generated
 *                 query intervals' bounds will fall. Both the low and high bounds of each query
 *                 interval will be within this specified range.
 * @param numberOfQueries The number of query intervals to generate.
 * @param queryTypeInfo The type probability information used for generating query interval
 * bounds.
 * @param seed A seed value for random number generation.
 * @return A vector of pairs, where each pair consists of two SBEValue representing the low and
 * high bounds of an interval.
 */
std::vector<std::pair<stats::SBEValue, stats::SBEValue>> generateIntervals(
    QueryType queryType,
    const std::pair<size_t, size_t>& interval,
    size_t numberOfQueries,
    const TypeCombination& queryTypeInfo,
    size_t seedQueriesLow,
    size_t seedQueriesHigh,
    boost::optional<size_t> ndv = boost::none);

std::vector<std::vector<std::pair<stats::SBEValue, stats::SBEValue>>> generateMultiFieldIntervals(
    WorkloadConfiguration queryConfig);

/**
 * Helper function for CE accuracy and performance benchmarks for checking types in generated
 * datasets.
 * Checks the membership of the first argument (checkType) in the provided vector
 * (typesInData).
 * The benchmarks assumes that Arrays contain only Integer types.
 *
 * @param checkType The data type queries are using.
 * @param typesInData The data types included in the dataset.
 * @return Boolean value, true if the query data type is present in the dataset data types,
 * false otherwise.
 */
bool checkTypeExistence(const sbe::value::TypeTags& checkType, const TypeCombination& typesInData);

/**
 * Helpful macros for asserting that the CE of a $match predicate is approximately what we were
 * expecting.
 */

#define _ASSERT_CE(estimatedCE, expectedCE)                             \
    if constexpr (kCETestLogOnly) {                                     \
        if (!nearlyEqual(estimatedCE, expectedCE, kMaxCEError)) {       \
            std::cout << "ERROR: expected " << expectedCE << std::endl; \
        }                                                               \
        ASSERT_APPROX_EQUAL(1.0, 1.0, kMaxCEError);                     \
    } else {                                                            \
        ASSERT_APPROX_EQUAL(estimatedCE, expectedCE, kMaxCEError);      \
    }
#define _PREDICATE(field, predicate) (str::stream() << "{" << field << ": " << predicate "}")
#define _ELEMMATCH_PREDICATE(field, predicate) \
    (str::stream() << "{" << field << ": {$elemMatch: " << predicate << "}}")

// This macro verifies the cardinality of a pipeline or an input ABT.
#define ASSERT_CE(ce, pipeline, expectedCE) _ASSERT_CE(ce.getCE(pipeline), (expectedCE))

// This macro does the same as above but also sets the collection cardinality.
#define ASSERT_CE_CARD(ce, pipeline, expectedCE, collCard) \
    ce.setCollCard({collCard});                            \
    ASSERT_CE(ce, pipeline, expectedCE)

// This macro verifies the cardinality of a pipeline with a single $match predicate.
#define ASSERT_MATCH_CE(ce, predicate, expectedCE) \
    _ASSERT_CE(ce.getMatchCE(predicate), (expectedCE))

#define ASSERT_MATCH_CE_NODE(ce, queryPredicate, expectedCE, nodePredicate) \
    _ASSERT_CE(ce.getMatchCE(queryPredicate, nodePredicate), (expectedCE))

// This macro does the same as above but also sets the collection cardinality.
#define ASSERT_MATCH_CE_CARD(ce, predicate, expectedCE, collCard) \
    ce.setCollCard({collCard});                                   \
    ASSERT_MATCH_CE(ce, predicate, expectedCE)

// This macro tests cardinality of two versions of the predicate; with and without $elemMatch.
#define ASSERT_EQ_ELEMMATCH_CE(tester, expectedCE, elemMatchExpectedCE, field, predicate) \
    ASSERT_MATCH_CE(tester, _PREDICATE(field, predicate), expectedCE);                    \
    ASSERT_MATCH_CE(tester, _ELEMMATCH_PREDICATE(field, predicate), elemMatchExpectedCE)

#define ASSERT_EQ_ELEMMATCH_CE_NODE(tester, expectedCE, elemMatchExpectedCE, field, predicate, n) \
    ASSERT_MATCH_CE_NODE(tester, _PREDICATE(field, predicate), expectedCE, n);                    \
    ASSERT_MATCH_CE_NODE(tester, _ELEMMATCH_PREDICATE(field, predicate), elemMatchExpectedCE, n)


}  // namespace mongo::ce
