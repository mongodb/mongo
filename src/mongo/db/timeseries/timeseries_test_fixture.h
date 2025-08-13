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
#include "mongo/base/string_data_comparator.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/local_catalog/catalog_test_fixture.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/service_context.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_catalog.h"
#include "mongo/db/timeseries/timeseries_options.h"
#include "mongo/platform/decimal128.h"
#include "mongo/util/uuid.h"

#include <functional>
#include <utility>
#include <vector>

namespace mongo::timeseries {
class TimeseriesTestFixture : public CatalogTestFixture {
public:
    static constexpr uint64_t kDefaultStorageCacheSizeBytes = 1024 * 1024 * 1024;
    static constexpr uint64_t kLimitedStorageCacheSizeBytes = 1024;

protected:
    void setUp() override;
    void tearDown() override;

    virtual BSONObj _makeTimeseriesOptionsForCreate() const;

    virtual BSONObj _makeTimeseriesOptionsForCreateNoMetaField() const;

    virtual void setUpCollectionsHelper(
        std::initializer_list<std::pair<NamespaceString*, UUID*>> collectionMetadata,
        std::function<BSONObj()> makeTimeseriesOptionsForCreateFn);

    virtual void validateCollectionsHelper(const std::set<NamespaceString>& collections);

    // Ensure that the input collection has a meta field and that at least one measurement has a
    // meta field value.
    void _assertCollWithMetaField(const NamespaceString& ns,
                                  std::vector<BSONObj> batchOfMeasurements) const;

    void _assertCollWithoutMetaField(const NamespaceString& ns) const;

    // Ensure that the input collection is configured with a meta field. Ensure that no measurements
    // in the collection have meta field values.
    void _assertNoMetaFieldsInCollWithMetaField(const NamespaceString& ns,
                                                std::vector<BSONObj> batchOfMeasurements) const;

    TimeseriesOptions _getTimeseriesOptions(const NamespaceString& ns) const;

    const CollatorInterface* _getCollator(const NamespaceString& ns) const;

    // _generateMeasurement enables the user to generate a BSONObjBuilder with the input type and
    // timeValue. It is the caller's responsibility to use .obj() from the returned BSONObjBuilder
    // to use the BSONObj measurement.
    //
    // This variant of _generateMeasurement gives default values for each input 'type'.
    //
    // There are simplifications with the Array, Object, RegEx, DBRef, and CodeWScope
    // BSONTypes (we use the 'metaValue' in a String component of the BSONElement rather than
    // configuring the entire BSONType output).
    //
    // Set timeValue to boost::none to generate a malformed measurement.
    BSONObjBuilder _generateMeasurement(boost::optional<BSONType> type,
                                        boost::optional<Date_t> timeValue = Date_t::now()) const;

    // We don't supply defaults for these type-specific function declarations because we provide the
    // defaults in the generic _generateMeasurement above.
    BSONObjBuilder _generateMeasurement(const boost::optional<BSONObj>& metaValue,
                                        boost::optional<Date_t> timeValue = Date_t::now()) const;

    bucket_catalog::Bucket* _generateBucketWithBatch(const NamespaceString& ns,
                                                     const UUID& collectionUUID,
                                                     bucket_catalog::BatchedInsertContext& batch,
                                                     size_t batchIdx = 0) const;

    /**
     * Without inserting 'batch', asserts that 'batch' doesn't rollover 'bucket'.
     */
    void _assertBatchDoesNotRollover(const TimeseriesOptions& options,
                                     bucket_catalog::BatchedInsertContext& batch,
                                     bucket_catalog::Bucket* bucket);
    /**
     * Performs stageInsertBatchIntoEligibleBucket.
     * Before staging 'batch', we check that the 'batch' doesn't rollover due to any reason and will
     * assert otherwise.
     */
    void _stageInsertOneBatchIntoEligibleBucketHelper(const NamespaceString& ns,
                                                      const UUID& collectionUUID,
                                                      bucket_catalog::BatchedInsertContext& batch,
                                                      bucket_catalog::Bucket* bucket);

    /**
     * Creates a vector of buckets. The i-th bucket contains the measurements and rollover reason
     * from measurementsAndRolloverReasons[i].
     * We require that for the list of measurements in measurementsAndRolloverReasons[i], we
     * will not rollover and will assert otherwise.
     * We require that measurementsAndRolloverReasons have the same meta value or the collection has
     * no meta value and will assert otherwise.
     * We require that there is no kNone reason unless there is only one bucket being created  in
     * measurementsAndRolloverReasons because for buckets with the same meta field/collections with
     * no meta field value, there can only be one uncleared kNone bucket. If we call
     * allocateBucket with an uncleared kNone bucket, we will invariant.
     * It is the responsibility of the caller of this function to set/unset the returned bucket's
     * rollover reasons before staging/committing writes.
     */
    absl::InlinedVector<bucket_catalog::Bucket*, 8> _generateBucketsWithMeasurements(
        const NamespaceString& ns,
        const UUID& collectionUUID,
        const std::vector<std::pair<std::vector<BSONObj>, bucket_catalog::RolloverReason>>&
            measurementsAndRolloverReasons);

    /*
     * Creates 'numBuckets' buckets that are full (gTimeseriesBucketMaxCount measurements) with the
     * measurement having the 'metaValue' field.
     * Non-meta field measurements can be created by passing in boost::none for 'metaValueType'.
     */
    absl::InlinedVector<bucket_catalog::Bucket*, 8> _createFullBuckets(
        const NamespaceString& ns,
        const UUID& collectionUUID,
        size_t numBuckets,
        boost::optional<BSONObj> metaValue,
        boost::optional<BSONType> metaValueType);

    struct MeasurementsWithRolloverReasonOptions {
        const bucket_catalog::RolloverReason reason;
        size_t numMeasurements = static_cast<size_t>(gTimeseriesBucketMaxCount);
        size_t idxWithDiffMeasurement = static_cast<size_t>(numMeasurements - 1);
        Date_t timeValue = Date_t::now();
        boost::optional<BSONObj> metaValue = boost::none;
        boost::optional<BSONType> metaValueType = BSONType::string;
        size_t extraPayload = 0;  // padding on rollover measurement
    };

    // _generateMeasurementsWithRolloverReason enables us to easily get measurement vectors that
    // have the input RolloverReason. Input conditions: numMeasurements is the number of
    // measurements that should be returned in the measurement vector.
    // Default: gTimeseriesBucketMaxCount
    // 1 <= 'numMeasurements' <= gTimeseriesBucketMaxCount    if kNone
    // 2 <= 'numMeasurements' <= gTimeseriesBucketMaxCount    if kSchemaChange,
    //                                                           kTimeForward,
    //                                                           kTimeBackward
    // cannot set 'numMeasurements'                           otherwise
    //
    // 'idxWithDiffMeasurement' is the index where we change the record in a measurement vector to
    // simulate a specific rollover reason. This is only used for kSchemaChange, kTimeForward,
    // kTimeBackward.
    // Default: numMeasurements - 1
    // 1 <= 'idxWithDiffMeasurement' <= numMeasurements       if kSchemaChange,
    //                                                           kTimeForward,
    //                                                           kTimeBackward
    // cannot set 'idxWithDiffMeasurement'                    otherwise
    //
    // 'metaValue' and 'timeValue' are what we set as the meta value and time value for a
    // measurement.
    // The default metaValue is set to being a String _metaValue: {_metaField: _metaValue}. Passing
    // in a 'metaValue' takes precedence over passing in a 'metaValueType'.
    // If we pass in a 'metaValue', we will choose generating the measurement with that meta value.
    // over the default meta value for the input 'metaValueType'.The 'metaValue' passed in must be a
    // well-formed BSONObj that has the following structure:
    // {_metaField << metaValue}
    // Notably, 'metaValue' must have a field _metaField and only one field. We will invariant
    // otherwise.
    // To form a measurement without a meta value, you must set 'metaValueType' = boost::none.
    // As mentioned above, if you specify a 'metaValue' with 'metaValueType' == boost::none, the
    // 'metaValue' will take precedence of being set as the meta value.s
    // The default 'timeValue' is Date_t::now().
    std::vector<BSONObj> _generateMeasurementsWithRolloverReason(
        const MeasurementsWithRolloverReasonOptions& options);

    uint64_t _getStorageCacheSizeBytes() const;

    long long _getExecutionStat(const UUID& uuid, StringData stat);

    template <typename T>
    inline std::vector<T> _getFlattenedVector(const std::vector<std::vector<T>>& vectors) {
        size_t totalSize = std::accumulate(
            vectors.begin(), vectors.end(), size_t(0), [](size_t sum, const std::vector<T>& vec) {
                return sum + vec.size();
            });
        std::vector<T> result;
        result.reserve(totalSize);  // Reserve the total size to avoid multiple allocations
        // Use a range-based for loop to insert elements
        for (const auto& vec : vectors) {
            result.insert(result.end(), vec.begin(), vec.end());
        }
        return result;
    }

    OperationContext* _opCtx;
    bucket_catalog::BucketCatalog* _bucketCatalog;

    static constexpr StringData _timeField = "time";
    static constexpr StringData _metaField = "tag";
    static constexpr StringData _metaValue = "a";
    static constexpr StringData _metaValue2 = "b";
    static constexpr StringData _metaValue3 = "c";
    uint64_t _storageCacheSizeBytes = kDefaultStorageCacheSizeBytes;

    const std::vector<BSONType> _nonStringComponentVariableBSONTypes = {BSONType::timestamp,
                                                                        BSONType::date,
                                                                        BSONType::numberInt,
                                                                        BSONType::numberLong,
                                                                        BSONType::numberDecimal,
                                                                        BSONType::numberDouble,
                                                                        BSONType::oid,
                                                                        BSONType::boolean,
                                                                        BSONType::binData};

    const std::vector<BSONType> _stringComponentBSONTypes = {BSONType::object,
                                                             BSONType::array,
                                                             BSONType::regEx,
                                                             BSONType::dbRef,
                                                             BSONType::code,
                                                             BSONType::symbol,
                                                             BSONType::codeWScope,
                                                             BSONType::string};

    // These BSONTypes will always return the same meta value when passed in as the BSONType in
    // _generateMeasurement.
    const std::vector<BSONType> _constantBSONTypes = {
        BSONType::undefined, BSONType::minKey, BSONType::maxKey, BSONType::null, BSONType::eoo};

    // Strings used to simulate kSize/kCachePressure rollover reason.
    std::string _bigStr = std::string(1000, 'a');

    NamespaceString _ns1 =
        NamespaceString::createNamespaceString_forTest("timeseries_test_fixture_1", "t_1");
    NamespaceString _ns2 =
        NamespaceString::createNamespaceString_forTest("timeseries_test_fixture_1", "t_2");
    NamespaceString _nsNoMeta = NamespaceString::createNamespaceString_forTest(
        "timeseries_test_fixture_no_meta_field_1", "t_1");

    std::set<NamespaceString> _collections = {_ns1, _ns2, _nsNoMeta};

    // Helper function to add a collection ns to be validated in TimeseriesTestFixture::tearDown().
    // Used when _ns1, _ns2, or _nsNoMeta shouldn't be used due to using different collection
    // configurations.
    void _addNsToValidate(const NamespaceString& ns);

    // Stubs for testing, will not be called.
    bucket_catalog::CompressAndWriteBucketFunc _compressBucketFuncUnused = nullptr;
    StringDataComparator* _stringDataComparatorUnused = nullptr;

    UUID _uuid1 = UUID::gen();
    UUID _uuid2 = UUID::gen();
    UUID _uuidNoMeta = UUID::gen();

    BSONObj _measurement = BSON(_timeField << Date_t::now() << _metaField << _metaValue);

    BSONObj _undefinedMeta = BSON(_metaField << BSONUndefined);
    BSONObj _minKeyMeta = BSON(_metaField << MINKEY);
    BSONObj _maxKeyMeta = BSON(_metaField << MAXKEY);
    BSONObj _nullMeta = BSON(_metaField << BSONNULL);
    BSONObj _eooMeta = BSON(_metaField << BSONObj());
    StatusWith<Date_t> date = dateFromISOString("2022-06-06T15:34:00.000Z");
    BSONObj _dateMeta = BSON(_metaField << date.getValue());
    BSONObj _bsonTimestampMeta = BSON(_metaField << Timestamp(1, 2));
    BSONObj _intMeta = BSON(_metaField << 365);
    BSONObj _longMeta = BSON(_metaField << static_cast<long long>(0x0123456789abcdefll));
    BSONObj _decimalMeta = BSON(_metaField << Decimal128("0.30"));
    BSONObj _doubleMeta = BSON(_metaField << 1.5);
    BSONObj _oidMeta = BSON(_metaField << OID("dbdbdbdbdbdbdbdbdbdbdbdb"));
    BSONObj _boolMeta = BSON(_metaField << true);
    BSONObj _binDataMeta = BSON(_metaField << BSONBinData("", 0, BinDataGeneral));
    BSONObj _objMeta = BSON(_metaField << BSON("0" << _metaValue));
    BSONObj _arrayMeta = BSON(_metaField << BSONArray(BSON("0" << _metaValue)));
    BSONObj _regexMeta = BSON(_metaField << BSONRegEx(_metaValue, _metaValue));
    BSONObj _dbRefMeta = BSON(_metaField << BSONDBRef(_metaValue, OID("dbdbdbdbdbdbdbdbdbdbdbdb")));
    BSONObj _codeMeta = BSON(_metaField << BSONCode(_metaValue));
    BSONObj _symbolMeta = BSON(_metaField << BSONSymbol(_metaValue));
    BSONObj _codeWScopeMeta = BSON(_metaField << BSONCodeWScope(_metaValue, BSON("x" << 1)));
    BSONObj _stringMeta = BSON(_metaField << _metaValue);
};
}  // namespace mongo::timeseries
