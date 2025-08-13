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

#include "mongo/db/storage/mdb_catalog.h"

#include "mongo/bson/json.h"
#include "mongo/db/local_catalog/clustered_collection_util.h"
#include "mongo/db/local_catalog/collection_options.h"
#include "mongo/db/local_catalog/collection_record_store_options.h"
#include "mongo/db/local_catalog/durable_catalog.h"
#include "mongo/db/local_catalog/durable_catalog_entry_metadata.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

namespace mongo {

class MDBCatalogTest : public mongo::unittest::Test {
public:
    bool possessesAllRelevantElements(const BSONObj& expected, const BSONObj& actual) {
        for (BSONElement elem : expected) {
            BSONElement actualElem = actual[elem.fieldName()];
            if (elem.type() == mongo::BSONType::object) {
                if (actualElem.type() != mongo::BSONType::object ||
                    !possessesAllRelevantElements(elem.Obj(), actualElem.Obj()))
                    return false;
            } else {
                if (!actualElem.binaryEqual(elem))
                    return false;
            }
        }
        return true;
    }

    bool recordStoreOptionsAreMaintained(const RecordStore::Options& expected,
                                         const RecordStore::Options& actual) {
        if (expected.customBlockCompressor) {
            if (!actual.customBlockCompressor)
                return false;
            if (expected.customBlockCompressor->compare(*(actual.customBlockCompressor)) != 0)
                return false;
        }
        return expected.keyFormat == actual.keyFormat && expected.isCapped == actual.isCapped &&
            expected.isOplog == actual.isOplog && expected.oplogMaxSize == actual.oplogMaxSize &&
            expected.allowOverwrite == actual.allowOverwrite &&
            expected.forceUpdateWithFullDocument == actual.forceUpdateWithFullDocument &&
            possessesAllRelevantElements(expected.storageEngineCollectionOptions,
                                         actual.storageEngineCollectionOptions);
    }

    RecordStore::Options parseRecordStoreOptionsWithCatalog(const NamespaceString& nss,
                                                            const BSONObj& obj) {
        RecordStore::Options result;

        RecordId dummyId;
        auto catalogEntry = durable_catalog::parseCatalogEntry(dummyId, obj);
        auto md = catalogEntry->metadata;

        return getRecordStoreOptions(nss, md->options);
    }

    void testParseEquality(const char* config) {
        NamespaceString nss = NamespaceString::createNamespaceString_forTest("testNs.foo");
        NamespaceString nssOplog =
            NamespaceString::createNamespaceString_forTest("local.oplog.testNs");
        BSONObj configObj = mongo::fromjson(config);

        auto expected = parseRecordStoreOptionsWithCatalog(nss, configObj);
        auto actual = MDBCatalog::_parseRecordStoreOptions(nss, configObj);
        ASSERT(recordStoreOptionsAreMaintained(expected, actual));
        auto expectedOplog = parseRecordStoreOptionsWithCatalog(nssOplog, configObj);
        auto actualOplog = MDBCatalog::_parseRecordStoreOptions(nssOplog, configObj);
        ASSERT(recordStoreOptionsAreMaintained(expectedOplog, actualOplog));
    }

    BSONObj buildOrphanedCatalogEntryObjAndNs(const std::string& ident,
                                              bool isClustered,
                                              NamespaceString* nss,
                                              std::string* ns,
                                              UUID uuid = UUID::gen()) {
        return MDBCatalog::_buildOrphanedCatalogEntryObjAndNs(ident, isClustered, nss, ns, uuid);
    }
};

/**
 * Test that MDBCatalog::_buildCatalogEntryObj is equivalent to previous implementation
 * creating it through CollectionOptions and catalog
 */
TEST_F(MDBCatalogTest, BuildCatalogEntryObjAndNsEquivalence) {
    // Standardize UUID for comparison
    UUID uuid = UUID::gen();

    // Construct expected output using CollectionOptions and catalog logic
    CollectionOptions optionsWithUUID;
    optionsWithUUID.uuid.emplace(uuid);
    optionsWithUUID.clusteredIndex = clustered_util::makeDefaultClusteredIdIndex();
    std::string identNs = "test-Ident";
    std::replace(identNs.begin(), identNs.end(), '-', '_');
    auto expectedNss = NamespaceStringUtil::deserialize(
        DatabaseName::kLocal, NamespaceString::kOrphanCollectionPrefix + identNs);
    auto expectedNs = NamespaceStringUtil::serializeForCatalog(expectedNss);
    durable_catalog::CatalogEntryMetaData md =
        durable_catalog::internal::createMetaDataForNewCollection(expectedNss, optionsWithUUID);
    auto expected =
        durable_catalog::internal::buildRawMDBCatalogEntry("test-Ident", BSONObj(), md, expectedNs);

    // Build with MDBCatalog function and compare
    NamespaceString nss;
    std::string ns;
    BSONObj actual = buildOrphanedCatalogEntryObjAndNs("test-Ident", true, &nss, &ns, uuid);
    ASSERT_EQ(expectedNss, nss);
    ASSERT_EQ(expectedNs, ns);
    ASSERT(possessesAllRelevantElements(expected, actual));
}

/**
 * Test that MDBCatalog::_parseRecordStoreOptions is equivalent to previous implementation
 * extracting it from a parsed CatalogEntry
 */
TEST_F(MDBCatalogTest, ParseRecordStoreOptionsEquivalence) {
    const char simpleConfig[] =
        "{ \
        \"ident\" : \"testident\", \
        \"md\" : { \
            \"options\" : { \
                \"clusteredIndex\" : { \
                    \"v\" : 2, \
                    \"key\" : {\"_id\" : 1}, \
                    \"name\" : \"foo\", \
                    \"unique\" : true \
                }, \
                \"size\" : 1048576, \
                \"timeseries\" : { \
                    \"timeField\" : \"tf\", \
                    \"metaField\" : \"mf\", \
                    \"granularity\" : \"minutes\", \
                    \"bucketRoundingSeconds\" : 10, \
                    \"bucketMaxSpanSeconds\" : 10000 \
                }, \
                \"capped\" : true, \
                \"storageEngine\" : { \"doc1\" : {\"a\" : 1}, \"doc2\" : {\"a\" : 2}} \
            } \
        } \
    }";
    const char minimalConfig[] =
        "{ \
        \"ident\" : \"testident\", \
        \"md\" : { \
            \"options\" : { \
                \"size\" : 1024, \
                \"capped\" : false, \
                \"storageEngine\" : { \"doc1\" : {\"a\" : 0}} \
            } \
        } \
    }";
    const char onlyclusterConfig[] =
        "{ \
        \"ident\" : \"testident\", \
        \"md\" : { \
            \"options\" : { \
                \"clusteredIndex\" : { \
                    \"v\" : 2, \
                    \"key\" : {\"_id\" : 1}, \
                    \"name\" : \"foo\", \
                    \"unique\" : true \
                }, \
                \"size\" : 1048576, \
                \"capped\" : true, \
                \"storageEngine\" : { \"doc1\" : {\"a\" : 1}, \"doc2\" : {\"a\" : 2}} \
            } \
        } \
    }";
    const char onlytsConfig[] =
        "{ \
        \"ident\" : \"testident\", \
        \"md\" : { \
            \"options\" : { \
                \"size\" : 1048576, \
                \"timeseries\" : { \
                    \"timeField\" : \"tf\", \
                    \"metaField\" : \"mf\", \
                    \"granularity\" : \"minutes\", \
                    \"bucketRoundingSeconds\" : 10, \
                    \"bucketMaxSpanSeconds\" : 10000 \
                }, \
                \"capped\" : true, \
                \"storageEngine\" : { \"doc1\" : {\"a\" : 1}, \"doc2\" : {\"a\" : 2}} \
            } \
        } \
    }";
    const char booleanClusterConfig[] =
        "{ \
        \"ident\" : \"testident\", \
        \"md\" : { \
            \"options\" : { \
                \"clusteredIndex\" : true, \
                \"size\" : 1048576, \
                \"capped\" : true, \
                \"storageEngine\" : { \"doc1\" : {\"a\" : 1}, \"doc2\" : {\"a\" : 2}} \
            } \
        } \
    }";

    testParseEquality(simpleConfig);
    testParseEquality(minimalConfig);
    testParseEquality(onlyclusterConfig);
    testParseEquality(onlytsConfig);
    testParseEquality(booleanClusterConfig);
}

}  // namespace mongo
