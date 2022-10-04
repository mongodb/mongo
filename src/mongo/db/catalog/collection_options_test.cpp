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

#include "mongo/db/catalog/collection_options.h"

#include <limits>

#include "mongo/db/json.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
using unittest::assertGet;

void checkRoundTrip(const CollectionOptions& options) {
    CollectionOptions parsedOptions = assertGet(CollectionOptions::parse(options.toBSON()));
    ASSERT_BSONOBJ_EQ(options.toBSON(), parsedOptions.toBSON());
}

TEST(CollectionOptions, SimpleRoundTrip) {
    CollectionOptions options;
    checkRoundTrip(options);

    options.capped = true;
    options.cappedSize = 10240;
    options.cappedMaxDocs = 1111;
    checkRoundTrip(options);

    options.setNoIdIndex();
    checkRoundTrip(options);
}

TEST(CollectionOptions, Validate) {
    CollectionOptions options;
    ASSERT_OK(options.validateForStorage());

    options.storageEngine = fromjson("{storageEngine1: 1}");
    ASSERT_NOT_OK(options.validateForStorage());
}

TEST(CollectionOptions, Validator) {

    ASSERT_NOT_OK(CollectionOptions::parse(fromjson("{validator: 'notAnObject'}")).getStatus());
    CollectionOptions options =
        assertGet(CollectionOptions::parse(fromjson("{validator: {a: 1}}")));
    ASSERT_BSONOBJ_EQ(options.validator, fromjson("{a: 1}"));

    options.validator = fromjson("{b: 1}");
    ASSERT_BSONOBJ_EQ(options.toBSON()["validator"].Obj(), fromjson("{b: 1}"));

    CollectionOptions defaultOptions;
    ASSERT_BSONOBJ_EQ(defaultOptions.validator, BSONObj());
    ASSERT(!defaultOptions.toBSON()["validator"]);
}

TEST(CollectionOptions, ErrorBadSize) {
    ASSERT_NOT_OK(CollectionOptions::parse(fromjson("{capped: true, size: -1}")).getStatus());
    ASSERT_NOT_OK(CollectionOptions::parse(fromjson("{capped: false, size: -1}")).getStatus());
    ASSERT_NOT_OK(CollectionOptions::parse(
                      BSON("capped" << true << "size" << std::numeric_limits<long long>::min()))
                      .getStatus());
    ASSERT_NOT_OK(
        CollectionOptions::parse(BSON("capped" << true << "size" << (1LL << 62))).getStatus());
    ASSERT_NOT_OK(CollectionOptions::parse(
                      BSON("capped" << true << "size" << std::numeric_limits<long long>::max()))
                      .getStatus());
}

TEST(CollectionOptions, ErrorBadMax) {
    ASSERT_NOT_OK(
        CollectionOptions::parse(BSON("capped" << true << "max" << (1LL << 31))).getStatus());
}

TEST(CollectionOptions, CappedSizeNotRoundUpForAlignment) {
    serverGlobalParams.mutableFeatureCompatibility.setVersion(
        multiversion::FeatureCompatibilityVersion::kVersion_6_2);
    const long long kUnalignedCappedSize = 1000;
    const long long kAlignedCappedSize = 1000;

    // Check size rounds up to multiple of alignment.
    auto options = assertGet(
        CollectionOptions::parse((BSON("capped" << true << "size" << kUnalignedCappedSize))));

    ASSERT_EQUALS(options.capped, true);
    ASSERT_EQUALS(options.cappedSize, kAlignedCappedSize);
    ASSERT_EQUALS(options.cappedMaxDocs, 0);
}

TEST(CollectionOptions, CappedSizeRoundsUpForAlignment) {
    serverGlobalParams.mutableFeatureCompatibility.setVersion(
        multiversion::FeatureCompatibilityVersion::kVersion_6_0);
    const long long kUnalignedCappedSize = 1000;
    const long long kAlignedCappedSize = 1024;

    // Check size rounds up to multiple of alignment.
    auto options = assertGet(
        CollectionOptions::parse((BSON("capped" << true << "size" << kUnalignedCappedSize))));

    ASSERT_EQUALS(options.capped, true);
    ASSERT_EQUALS(options.cappedSize, kAlignedCappedSize);
    ASSERT_EQUALS(options.cappedMaxDocs, 0);
}

TEST(CollectionOptions, IgnoreSizeWrongType) {
    auto options =
        assertGet(CollectionOptions::parse(fromjson("{size: undefined, capped: undefined}")));
    ASSERT_EQUALS(options.capped, false);
    ASSERT_EQUALS(options.cappedSize, 0);
}

TEST(CollectionOptions, IgnoreMaxWrongType) {
    auto options =
        assertGet(CollectionOptions::parse(fromjson("{capped: true, size: 1024, max: ''}")));
    ASSERT_EQUALS(options.capped, true);
    ASSERT_EQUALS(options.cappedSize, 1024);
    ASSERT_EQUALS(options.cappedMaxDocs, 0);
}

TEST(CollectionOptions, InvalidStorageEngineField) {
    // "storageEngine" field has to be an object if present.
    ASSERT_NOT_OK(CollectionOptions::parse(fromjson("{storageEngine: 1}")).getStatus());

    // Every field under "storageEngine" has to be an object.
    ASSERT_NOT_OK(
        CollectionOptions::parse(fromjson("{storageEngine: {storageEngine1: 1}}")).getStatus());

    // Empty "storageEngine" not allowed
    ASSERT_OK(CollectionOptions::parse(fromjson("{storageEngine: {}}")).getStatus());
}

TEST(CollectionOptions, ParseEngineField) {
    auto opts = assertGet(CollectionOptions::parse(
        fromjson("{storageEngine: {storageEngine1: {x: 1, y: 2}, storageEngine2: {a: 1, b:2}}}")));
    checkRoundTrip(opts);

    BSONObj obj = opts.toBSON();

    // Check "storageEngine" field.
    ASSERT_TRUE(obj.hasField("storageEngine"));
    ASSERT_TRUE(obj.getField("storageEngine").isABSONObj());
    BSONObj storageEngine = obj.getObjectField("storageEngine");

    // Check individual storage storageEngine fields.
    ASSERT_TRUE(storageEngine.getField("storageEngine1").isABSONObj());
    BSONObj storageEngine1 = storageEngine.getObjectField("storageEngine1");
    ASSERT_EQUALS(1, storageEngine1.getIntField("x"));
    ASSERT_EQUALS(2, storageEngine1.getIntField("y"));

    ASSERT_TRUE(storageEngine.getField("storageEngine2").isABSONObj());
    BSONObj storageEngine2 = storageEngine.getObjectField("storageEngine2");
    ASSERT_EQUALS(1, storageEngine2.getIntField("a"));
    ASSERT_EQUALS(2, storageEngine2.getIntField("b"));
}

TEST(CollectionOptions, ResetStorageEngineField) {

    auto opts =
        assertGet(CollectionOptions::parse(fromjson("{storageEngine: {storageEngine1: {x: 1}}}")));
    checkRoundTrip(opts);

    CollectionOptions defaultOpts;
    ASSERT_TRUE(defaultOpts.storageEngine.isEmpty());
}

TEST(CollectionOptions, ModifyStorageEngineField) {
    CollectionOptions opts;

    // Directly modify storageEngine field in collection options.
    opts.storageEngine = fromjson("{storageEngine1: {x: 1}}");

    // Unrecognized field should not be present in BSON representation.
    BSONObj obj = opts.toBSON();
    ASSERT_FALSE(obj.hasField("unknownField"));

    // Check "storageEngine" field.
    ASSERT_TRUE(obj.hasField("storageEngine"));
    ASSERT_TRUE(obj.getField("storageEngine").isABSONObj());
    BSONObj storageEngine = obj.getObjectField("storageEngine");

    // Check individual storage storageEngine fields.
    ASSERT_TRUE(storageEngine.getField("storageEngine1").isABSONObj());
    BSONObj storageEngine1 = storageEngine.getObjectField("storageEngine1");
    ASSERT_EQUALS(1, storageEngine1.getIntField("x"));
}

TEST(CollectionOptions, FailToParseCollationThatIsNotAnObject) {
    ASSERT_NOT_OK(CollectionOptions::parse(fromjson("{collation: 'notAnObject'}")).getStatus());
}

TEST(CollectionOptions, FailToParseCollationThatIsAnEmptyObject) {
    ASSERT_NOT_OK(CollectionOptions::parse(fromjson("{collation: {}}")).getStatus());
}

TEST(CollectionOptions, CollationFieldParsesCorrectly) {
    auto options = assertGet(CollectionOptions::parse(fromjson("{collation: {locale: 'en'}}")));
    ASSERT_BSONOBJ_EQ(options.collation, fromjson("{locale: 'en'}"));
    ASSERT_OK(options.validateForStorage());
}

TEST(CollectionOptions, ParsedCollationObjShouldBeOwned) {
    auto options = assertGet(CollectionOptions::parse(fromjson("{collation: {locale: 'en'}}")));
    ASSERT_BSONOBJ_EQ(options.collation, fromjson("{locale: 'en'}"));
    ASSERT_TRUE(options.collation.isOwned());
}

TEST(CollectionOptions, ResetClearsCollationField) {
    CollectionOptions options;
    ASSERT_TRUE(options.collation.isEmpty());
    options = assertGet(CollectionOptions::parse(fromjson("{collation: {locale: 'en'}}")));
    ASSERT_FALSE(options.collation.isEmpty());
}

TEST(CollectionOptions, CollationFieldLeftEmptyWhenOmitted) {
    auto options = assertGet(CollectionOptions::parse(fromjson("{validator: {a: 1}}")));
    ASSERT_TRUE(options.collation.isEmpty());
}

TEST(CollectionOptions, CollationFieldNotDumpedToBSONWhenOmitted) {
    auto options = assertGet(CollectionOptions::parse(fromjson("{validator: {a: 1}}")));
    ASSERT_TRUE(options.collation.isEmpty());
    BSONObj asBSON = options.toBSON();
    ASSERT_FALSE(asBSON["collation"]);
}

TEST(CollectionOptions, ViewParsesCorrectly) {
    auto options =
        assertGet(CollectionOptions::parse(fromjson("{viewOn: 'c', pipeline: [{$match: {}}]}")));
    ASSERT_EQ(options.viewOn, "c");
    ASSERT_BSONOBJ_EQ(options.pipeline, fromjson("[{$match: {}}]"));
}

TEST(CollectionOptions, ViewParsesCorrectlyWithoutPipeline) {
    auto options = assertGet(CollectionOptions::parse(fromjson("{viewOn: 'c'}")));
    ASSERT_EQ(options.viewOn, "c");
    ASSERT_BSONOBJ_EQ(options.pipeline, BSONObj());
}

TEST(CollectionOptions, PipelineFieldRequiresViewOn) {
    ASSERT_NOT_OK(CollectionOptions::parse(fromjson("{pipeline: [{$match: {}}]}")).getStatus());
}

TEST(CollectionOptions, UnknownTopLevelOptionFailsToParse) {
    auto statusWith = CollectionOptions::parse(fromjson("{invalidOption: 1}"));
    ASSERT_EQ(statusWith.getStatus().code(), ErrorCodes::InvalidOptions);
}

TEST(CollectionOptions, CreateOptionIgnoredIfFirst) {
    ASSERT_OK(CollectionOptions::parse(fromjson("{create: 1}")).getStatus());
}

TEST(CollectionOptions, CreateOptionIgnoredIfNotFirst) {
    auto options =
        assertGet(CollectionOptions::parse(fromjson("{capped: true, create: 1, size: 1024}")));
    ASSERT_EQ(options.capped, true);
    ASSERT_EQ(options.cappedSize, 1024L);
}

TEST(CollectionOptions, UnknownOptionIgnoredIfCreateOptionFirst) {
    ASSERT_OK(CollectionOptions::parse(fromjson("{create: 1, invalidOption: 1}")).getStatus());
}

TEST(CollectionOptions, UnknownOptionIgnoredIfCreateOptionPresent) {
    ASSERT_OK(CollectionOptions::parse(fromjson("{invalidOption: 1, create: 1}")).getStatus());
}

TEST(CollectionOptions, UnknownOptionRejectedIfCreateOptionNotPresent) {
    auto statusWith = CollectionOptions::parse(fromjson("{invalidOption: 1}"));
    ASSERT_EQ(statusWith.getStatus().code(), ErrorCodes::InvalidOptions);
}

TEST(CollectionOptions, DuplicateCreateOptionIgnoredIfCreateOptionFirst) {
    auto statusWith = CollectionOptions::parse(BSON("create" << 1 << "create" << 1));
    ASSERT_OK(statusWith.getStatus());
}

TEST(CollectionOptions, DuplicateCreateOptionIgnoredIfCreateOptionNotFirst) {
    auto statusWith = CollectionOptions::parse(
        BSON("capped" << true << "create" << 1 << "create" << 1 << "size" << 1024));
    ASSERT_OK(statusWith.getStatus());
}

TEST(CollectionOptions, MaxTimeMSAllowlistedOptionIgnored) {
    auto statusWith = CollectionOptions::parse(fromjson("{maxTimeMS: 1}"));
    ASSERT_OK(statusWith.getStatus());
}

TEST(CollectionOptions, WriteConcernAllowlistedOptionIgnored) {
    auto statusWith = CollectionOptions::parse(fromjson("{writeConcern: 1}"));
    ASSERT_OK(statusWith.getStatus());
}

TEST(CollectionOptions, ParseUUID) {
    CollectionOptions options;
    UUID uuid = UUID::gen();

    // Check required parse failures
    ASSERT_FALSE(options.uuid);
    ASSERT_NOT_OK(CollectionOptions::parse(uuid.toBSON()).getStatus());
    ASSERT_NOT_OK(CollectionOptions::parse(BSON("uuid" << 1)).getStatus());
    ASSERT_NOT_OK(CollectionOptions::parse(BSON("uuid" << 1), CollectionOptions::parseForStorage)
                      .getStatus());

    // Check successful parse and roundtrip.
    options =
        assertGet(CollectionOptions::parse(uuid.toBSON(), CollectionOptions::parseForStorage));
    ASSERT(options.uuid.value() == uuid);

    // Check that a collection options containing a UUID passes validation.
    ASSERT_OK(options.validateForStorage());
}

TEST(CollectionOptions, SizeNumberLimits) {
    CollectionOptions options = assertGet(CollectionOptions::parse(fromjson("{size: 'a'}")));
    ASSERT_EQ(options.cappedSize, 0);

    options = assertGet(CollectionOptions::parse(fromjson("{size: '-1'}")));
    ASSERT_EQ(options.cappedSize, 0);

    options =
        assertGet(CollectionOptions::parse(fromjson("{size: '-9999999999999999999999999999999'}")));
    ASSERT_EQ(options.cappedSize, 0);

    // The test for size is redundant since size returns a status that's not ok if it's larger
    // than a petabyte, which is smaller than LLONG_MAX anyways. We test that here.
    ASSERT_NOT_OK(CollectionOptions::parse(fromjson("{size: 9999999999999999}")).getStatus());
}

TEST(CollectionOptions, MaxNumberLimits) {
    CollectionOptions options = assertGet(CollectionOptions::parse(fromjson("{max: 'a'}")));
    ASSERT_EQ(options.cappedMaxDocs, 0);

    options = assertGet(CollectionOptions::parse(fromjson("{max: '-1'}")));
    ASSERT_EQ(options.cappedMaxDocs, 0);

    options = assertGet(
        CollectionOptions::parse(fromjson("{max: '-9999999999999999999999999999999999'}")));
    ASSERT_EQ(options.cappedMaxDocs, 0);

    options = assertGet(CollectionOptions::parse(fromjson("{max: 99999999999999999999999999999}")));
    ASSERT_EQ(options.cappedMaxDocs, 0);
}

TEST(CollectionOptions, NExtentsNoError) {
    // Check that $nExtents does not cause an error for backwards compatability
    assertGet(CollectionOptions::parse(fromjson("{$nExtents: 'a'}")));
}

#define ASSERT_STATUS_CODE(CODE, EXPRESSION) ASSERT_EQUALS(CODE, (EXPRESSION).getStatus().code())

// Duplicate fields are not allowed
TEST(FLECollectionOptions, MultipleFields) {
    ASSERT_STATUS_CODE(6338402, CollectionOptions::parse(fromjson(R"({
    encryptedFields: {
        "fields": [
            {
                "path": "name.first",
                "keyId": { '$uuid': '11d58b8a-0c6c-4d69-a0bd-70c6d9befae9' },
                "bsonType": "string",
                "queries": {"queryType": "equality"}
            },
            {
                "path": "name.first",
                "keyId": { '$uuid': '5f34e99a-b214-451f-b6f6-d3d28e933d15' },
                "bsonType": "string",
                "queries": [{"queryType": "equality"}]
            }
        ]
    }})")));
}

// Duplicate key ids are bad, it breaks the design
TEST(FLECollectionOptions, DuplicateKeyIds) {
    ASSERT_STATUS_CODE(6338401, CollectionOptions::parse(fromjson(R"({
    encryptedFields: {
        "fields": [
            {
                "path": "name.first",
                "keyId": { '$uuid': '5f34e99a-b214-451f-b6f6-d3d28e933d15' },
                "bsonType": "string",
                "queries": {"queryType": "equality"}
            },
            {
                "path": "name.last",
                "keyId": { '$uuid': '5f34e99a-b214-451f-b6f6-d3d28e933d15' },
                "bsonType": "string",
                "queries": [{"queryType": "equality"}]
            }
        ]
    }})")));
}

TEST(FLECollectionOptions, NonConflictingPrefixes) {
    ASSERT_OK(CollectionOptions::parse(fromjson(R"({
    encryptedFields: {
        "fields": [
            {
                "path": "name",
                "keyId": { '$uuid': '11d58b8a-0c6c-4d69-a0bd-70c6d9befae9' },
                "bsonType": "string",
                "queries": {"queryType": "equality"}
            },
            {
                "path": "nameOther",
                "keyId": { '$uuid': '5f34e99a-b214-451f-b6f6-d3d28e933d15' },
                "bsonType": "string",
                "queries": [{"queryType": "equality"}]
            }
        ]
    }})"))
                  .getStatus());

    ASSERT_OK(CollectionOptions::parse(fromjson(R"({
    encryptedFields: {
        "fields": [
            {
                "path": "a.b.c",
                "keyId": { '$uuid': '11d58b8a-0c6c-4d69-a0bd-70c6d9befae9' },
                "bsonType": "string",
                "queries": {"queryType": "equality"}
            },
            {
                "path": "a.b.cde",
                "keyId": { '$uuid': '5f34e99a-b214-451f-b6f6-d3d28e933d15' },
                "bsonType": "string",
                "queries": [{"queryType": "equality"}]
            }
        ]
    }})"))
                  .getStatus());
}

TEST(FLECollectionOptions, ConflictingPrefixes) {
    ASSERT_STATUS_CODE(6338403, CollectionOptions::parse(fromjson(R"({
    encryptedFields: {
        "fields": [
            {
                "path": "name",
                "keyId": { '$uuid': '11d58b8a-0c6c-4d69-a0bd-70c6d9befae9' },
                "bsonType": "string",
                "queries": {"queryType": "equality"}
            },
            {
                "path": "name.first",
                "keyId": { '$uuid': '5f34e99a-b214-451f-b6f6-d3d28e933d15' },
                "bsonType": "string",
                "queries": [{"queryType": "equality"}]
            }
        ]
    }})")));

    ASSERT_STATUS_CODE(6338403, CollectionOptions::parse(fromjson(R"({
    encryptedFields: {
        "fields": [
            {
                "path": "a.b.c",
                "keyId": { '$uuid': '11d58b8a-0c6c-4d69-a0bd-70c6d9befae9' },
                "bsonType": "string",
                "queries": {"queryType": "equality"}
            },
            {
                "path": "a.b.c.d.e",
                "keyId": { '$uuid': '5f34e99a-b214-451f-b6f6-d3d28e933d15' },
                "bsonType": "string",
                "queries": [{"queryType": "equality"}]
            }
        ]
    }})")));

    ASSERT_STATUS_CODE(6338403, CollectionOptions::parse(fromjson(R"({
    encryptedFields: {
        "fields": [
            {
                "path": "a.b.c.d.e.f",
                "keyId": { '$uuid': '11d58b8a-0c6c-4d69-a0bd-70c6d9befae9' },
                "bsonType": "string",
                "queries": {"queryType": "equality"}
            },
            {
                "path": "a.b.c.d",
                "keyId": { '$uuid': '5f34e99a-b214-451f-b6f6-d3d28e933d15' },
                "bsonType": "string",
                "queries": [{"queryType": "equality"}]
            }
        ]
    }})")));
}

TEST(FLECollectionOptions, DuplicateQueryTypes) {
    ASSERT_STATUS_CODE(6338404, CollectionOptions::parse(fromjson(R"({
    encryptedFields: {
        "fields": [
            {
                "path": "name.first",
                "keyId": { '$uuid': '5f34e99a-b214-451f-b6f6-d3d28e933d15' },
                "bsonType": "string",
                "queries": [{"queryType": "equality"}, {"queryType": "equality"}]
            }
        ]
    }})")));
}

TEST(FLECollectionOptions, Equality_AllowedTypes) {
    std::vector<std::string> typesAllowedIndexed({
        "string",
        "binData",
        "objectId",
        "bool",
        "date",
        "regex",
        "javascript",
        "int",
        "timestamp",
        "long",
        "dbPointer",
        "symbol",
    });

    std::vector<std::string> typesAllowedUnindexed({
        "string",
        "binData",
        "objectId",
        "bool",
        "date",
        "regex",
        "javascript",
        "int",
        "timestamp",
        "long",
        "double",
        "object",
        "array",
        "decimal",
        "dbPointer",
        "symbol",
        "javascriptWithScope",
    });

    for (const auto& type : typesAllowedIndexed) {
        ASSERT_OK(CollectionOptions::parse(fromjson(str::stream() << R"({
        encryptedFields: {
            "fields": [
                {
                    "path": "firstName",
                    "keyId": { '$uuid': '5f34e99a-b214-451f-b6f6-d3d28e933d15' },
                    "bsonType": ")" << type << R"(",
                    "queries": {"queryType": "equality"}
                }
            ]
        }
    }})"))
                      .getStatus());
    }

    for (const auto& type : typesAllowedUnindexed) {
        ASSERT_OK(CollectionOptions::parse(fromjson(str::stream() << R"({
    encryptedFields: {
        "fields": [
            {
                "path": "name.first",
                "keyId": { '$uuid': '5f34e99a-b214-451f-b6f6-d3d28e933d15' },
                "bsonType": ")" << type << R"("
            }
        ]
    }})"))
                      .getStatus());
    }
}


TEST(FLECollectionOptions, Equality_DisAllowedTypes) {
    std::vector<std::string> typesDisallowedIndexed({
        "minKey",
        "double",
        "object",
        "array",
        "null",
        "undefined",
        "javascriptWithScope",
        "decimal",
        "maxKey",
    });

    std::vector<std::string> typesDisallowedUnindexed({
        "minKey",
        "null",
        "undefined",
        "maxKey",
    });

    for (const auto& type : typesDisallowedIndexed) {
        ASSERT_STATUS_CODE(6338405, CollectionOptions::parse(fromjson(str::stream() << R"({
        encryptedFields: {
            "fields": [
                {
                    "path": "firstName",
                    "keyId": { '$uuid': '5f34e99a-b214-451f-b6f6-d3d28e933d15' },
                    "bsonType": ")" << type << R"(",
                    "queries": {"queryType": "equality"}
                }
            ]
        }
    }})")));
    }

    for (const auto& type : typesDisallowedUnindexed) {
        ASSERT_STATUS_CODE(6338406, CollectionOptions::parse(fromjson(str::stream() << R"({
    encryptedFields: {
        "fields": [
            {
                "path": "name.first",
                "keyId": { '$uuid': '5f34e99a-b214-451f-b6f6-d3d28e933d15' },
                "bsonType": ")" << type << R"("
            }
        ]
    }})")));
    }
}


TEST(FLECollectionOptions, Range_AllowedTypes) {
    // TODO: SERVER-67760 remove feature flag
    RAIIServerParameterControllerForTest featureFlagController("featureFlagFLE2Range", true);

    std::vector<std::string> typesAllowedIndexed({
        "int",
        "long",
        "double",
        "decimal",
    });


    for (const auto& type : typesAllowedIndexed) {
        ASSERT_OK(CollectionOptions::parse(fromjson(str::stream() << R"({
        encryptedFields: {
            "fields": [
                {
                    "path": "firstName",
                    "keyId": { '$uuid': '5f34e99a-b214-451f-b6f6-d3d28e933d15' },
                    "bsonType": ")" << type << R"(",
                    "queries": {"queryType": "range", "sparsity" : 1, min : 1, max : 2}
                }
            ]
        }
    }})"))
                      .getStatus());
    }

    // Validate date works
    ASSERT_OK(CollectionOptions::parse(fromjson(str::stream() << R"({
        encryptedFields: {
            "fields": [
                {
                    "path": "firstName",
                    "keyId": { '$uuid': '5f34e99a-b214-451f-b6f6-d3d28e933d15' },
                    "bsonType": "date",
                    "queries": {"queryType": "range", "sparsity" : 1, min : {"$date": {"$numberLong": "12344"}}, max : {"$date": {"$numberLong": "12345"}}}
                }
            ]
        }
    }})"))
                  .getStatus());
}


TEST(FLECollectionOptions, Range_DisAllowedTypes) {
    // TODO: SERVER-67760 remove feature flag
    RAIIServerParameterControllerForTest featureFlagController("featureFlagFLE2Range", true);

    std::vector<std::string> typesDisallowedIndexed({
        "array",
        "binData",
        "bool",
        "dbPointer",
        "javascript",
        "javascriptWithScope",
        "maxKey",
        "minKey",
        "null",
        "object",
        "objectId",
        "regex",
        "string",
        "symbol",
        "timestamp",
    });

    for (const auto& type : typesDisallowedIndexed) {
        ASSERT_STATUS_CODE(6775201, CollectionOptions::parse(fromjson(str::stream() << R"({
        encryptedFields: {
            "fields": [
                {
                    "path": "firstName",
                    "keyId": { '$uuid': '5f34e99a-b214-451f-b6f6-d3d28e933d15' },
                    "bsonType": ")" << type << R"(",
                    "queries": {"queryType": "range"}
                }
            ]
        }
    }})")));
    }
}

TEST(FLECollectionOptions, Range_MissingFields) {
    // TODO: SERVER-67760 remove feature flag
    RAIIServerParameterControllerForTest featureFlagController("featureFlagFLE2Range", true);

    ASSERT_STATUS_CODE(6775202, CollectionOptions::parse(fromjson(R"({
        encryptedFields: {
            "fields": [
                {
                    "path": "firstName",
                    "keyId": { '$uuid': '5f34e99a-b214-451f-b6f6-d3d28e933d15' },
                    "bsonType": "int",
                    "queries": {"queryType": "range"}
                }
            ]
        }
    }})")));

    ASSERT_STATUS_CODE(6775203, CollectionOptions::parse(fromjson(R"({
        encryptedFields: {
            "fields": [
                {
                    "path": "firstName",
                    "keyId": { '$uuid': '5f34e99a-b214-451f-b6f6-d3d28e933d15' },
                    "bsonType": "int",
                    "queries": {"queryType": "range", sparsity: 1}
                }
            ]
        }
    }})")));

    ASSERT_STATUS_CODE(6775204, CollectionOptions::parse(fromjson(R"({
        encryptedFields: {
            "fields": [
                {
                    "path": "firstName",
                    "keyId": { '$uuid': '5f34e99a-b214-451f-b6f6-d3d28e933d15' },
                    "bsonType": "int",
                    "queries": {"queryType": "range", sparsity: 1, min : 1}
                }
            ]
        }
    }})")));
}

TEST(FLECollectionOptions, Equality_ExtraFields) {
    // TODO: SERVER-67760 remove feature flag
    RAIIServerParameterControllerForTest featureFlagController("featureFlagFLE2Range", true);

    ASSERT_STATUS_CODE(6775205, CollectionOptions::parse(fromjson(R"({
        encryptedFields: {
            "fields": [
                {
                    "path": "firstName",
                    "keyId": { '$uuid': '5f34e99a-b214-451f-b6f6-d3d28e933d15' },
                    "bsonType": "int",
                    "queries": {"queryType": "equality", sparsity:1}
                }
            ]
        }
    }})")));

    ASSERT_STATUS_CODE(6775206, CollectionOptions::parse(fromjson(R"({
        encryptedFields: {
            "fields": [
                {
                    "path": "firstName",
                    "keyId": { '$uuid': '5f34e99a-b214-451f-b6f6-d3d28e933d15' },
                    "bsonType": "int",
                    "queries": {"queryType": "equality", min:1}
                }
            ]
        }
    }})")));

    ASSERT_STATUS_CODE(6775207, CollectionOptions::parse(fromjson(R"({
        encryptedFields: {
            "fields": [
                {
                    "path": "firstName",
                    "keyId": { '$uuid': '5f34e99a-b214-451f-b6f6-d3d28e933d15' },
                    "bsonType": "int",
                    "queries": {"queryType": "equality", max:1}
                }
            ]
        }
    }})")));
}


TEST(FLECollectionOptions, Range_MinMax) {
    // TODO: SERVER-67760 remove feature flag
    RAIIServerParameterControllerForTest featureFlagController("featureFlagFLE2Range", true);

    std::vector<std::pair<std::string, int>> types{
        {"int", 6775208},
        {"long", 6775209},
        {"double", 6775210},
        {"decimal", 6775211},
    };

    for (auto const& tc : types) {
        auto doc = BSON("encryptedFields"
                        << BSON("fields" << BSON_ARRAY(BSON(
                                    "path"
                                    << "firstName"
                                    << "keyId" << UUID::gen() << "bsonType" << tc.first << "queries"
                                    << BSON("queryType"
                                            << "range"
                                            << "sparsity" << 1 << "min" << 2 << "max" << 1)))));

        ASSERT_STATUS_CODE(tc.second, CollectionOptions::parse(doc));
    }


    Date_t start = Date_t::now();
    Date_t end = start;
    end += Hours(1);
    auto doc = BSON("encryptedFields"
                    << BSON("fields" << BSON_ARRAY(BSON("path"
                                                        << "firstName"
                                                        << "keyId" << UUID::gen() << "bsonType"
                                                        << "date"
                                                        << "queries"
                                                        << BSON("queryType"
                                                                << "range"
                                                                << "sparsity" << 1 << "min" << end
                                                                << "max" << start)))));

    ASSERT_STATUS_CODE(6775213, CollectionOptions::parse(doc));
}

}  // namespace mongo
