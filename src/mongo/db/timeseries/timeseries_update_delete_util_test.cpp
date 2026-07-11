// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/timeseries/timeseries_update_delete_util.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/json.h"
#include "mongo/db/client.h"
#include "mongo/db/timeseries/timeseries_test_fixture.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <boost/move/utility_core.hpp>
#include <fmt/format.h>

namespace mongo {
namespace {

class TimeseriesUpdateDeleteUtilTest : public timeseries::TimeseriesTestFixture {
protected:
    BSONObj _toBSON(const char* obj) const {
        return fromjson(fmt::format(fmt::runtime(obj), _metaField));
    }

    BSONObj _translateQuery(const char* query) const {
        return timeseries::translateQuery(_toBSON(query), _metaField);
    }

    BSONObj _translateUpdate(const BSONObj& update) const {
        return uassertStatusOK(
                   timeseries::translateUpdate(
                       write_ops::UpdateModification::parseFromClassicUpdate(update), _metaField))
            .getUpdateModifier();
    }

    BSONObj _translateUpdate(const char* update) const {
        return _translateUpdate(_toBSON(update));
    }

    /**
     * Runs two translations: the first is expected to succeed and the second is expected to fail.
     * Each translation can have three types of string substitutions.
     *
     * The first type of substitution, {0}, will be the metaField in both cases. On success, it is
     * expected to be translated to "meta".
     *
     * The second type of substitution, {1}, will be the metaField in the success case and will be
     * "notMetaField" in the fail case. On success, it is expected to be translated to "meta".
     *
     * The third type of substitution, {2}, will be the metaField in both cases. On success, it is
     * expected to be untranslated and still remain the metaField.
     */
    void _testTranslate(const char* obj, std::function<BSONObj(const BSONObj&)> translateFn) const {
        ASSERT_BSONOBJ_EQ(translateFn(fromjson(
                              fmt::format(fmt::runtime(obj), _metaField, _metaField, _metaField))),
                          fromjson(fmt::format(fmt::runtime(obj), "meta", "meta", _metaField)));

        ASSERT_THROWS_CODE(translateFn(fromjson(fmt::format(
                               fmt::runtime(obj), _metaField, "notMetaField", _metaField))),
                           AssertionException,
                           ErrorCodes::InvalidOptions);
    }

    void _testTranslateQuery(const char* query) const {
        _testTranslate(query, [metaField = _metaField](const BSONObj& query) {
            return timeseries::translateQuery(query, metaField);
        });
    }

    void _testTranslateUpdate(const char* update) const {
        _testTranslate(update, [this](const BSONObj& update) { return _translateUpdate(update); });
    }
};

TEST_F(TimeseriesUpdateDeleteUtilTest, TranslateQueryEmpty) {
    ASSERT_BSONOBJ_EQ(timeseries::translateQuery(BSONObj(), _metaField), BSONObj());
}

TEST_F(TimeseriesUpdateDeleteUtilTest, TranslateQuerySimple) {
    _testTranslateQuery(R"({{
        '{1}': 'A'
    }})");
}

TEST_F(TimeseriesUpdateDeleteUtilTest, TranslateQueryDotNotation) {
    _testTranslateQuery(R"({{
        '{1}.a': 'A'
    }})");
}

TEST_F(TimeseriesUpdateDeleteUtilTest, TranslateQueryPrefix) {
    ASSERT_THROWS_CODE(_translateQuery(R"({{
        '{0}a': 'A'
    }})"),
                       AssertionException,
                       ErrorCodes::InvalidOptions);
}

TEST_F(TimeseriesUpdateDeleteUtilTest, TranslateQueryNested) {
    _testTranslateQuery(R"({{
        '{1}': {{'{2}': 'A'}}
    }})");

    ASSERT_THROWS_CODE(_translateQuery(R"({{
        'a': {{
            '{0}': 'a'
        }}
    }})"),
                       AssertionException,
                       ErrorCodes::InvalidOptions);
}

TEST_F(TimeseriesUpdateDeleteUtilTest, TranslateQueryMultipleFields) {
    _testTranslateQuery(R"({{
        '{0}.a': 'A',
        '{1}.b': {{'$gt': 0}}
    }})");
}

TEST_F(TimeseriesUpdateDeleteUtilTest, TranslateQueryLogicalOperator) {
    _testTranslateQuery(R"({{
        '$and': [
            {{'{0}.a': 'A', '{0}.b': {{'$gt': 0}}}},
            {{'{0}.c': 'C', '{1}.d': {{'$nin': [1, 2, 3]}}}}
        ]
    }})");
}

TEST_F(TimeseriesUpdateDeleteUtilTest, TranslateQueryLogicalOperatorNested) {
    _testTranslateQuery(R"({{
        '$and': [
            {{'$or': [
                {{'{0}.a': 'A', '{0}.b': {{'$gt': 0}}}},
                {{'{0}.c': 'C', '{0}.d': {{'$nin': [1, 2, 3]}}}}
            ]}},
            {{'$nor': [
                {{'{0}.e': {{'$exists': true}}, '{0}.f': 'F'}},
                {{'{0}.g': {{'{2}': 0}}, '{1}.h': 'H'}}
            ]}}
        ]
    }})");
}

TEST_F(TimeseriesUpdateDeleteUtilTest, TranslateQueryJsonSchemaRequired) {
    _testTranslateQuery(R"({{
        '$jsonSchema': {{
            'required': ['{1}']
        }}
    }})");


    ASSERT_THROWS_CODE(_translateQuery(R"({{
        '$jsonSchema': {{
            'required': ['{0}', 'notMetaField']
        }}
    }})"),
                       AssertionException,
                       ErrorCodes::InvalidOptions);

    ASSERT_THROWS_CODE(_translateQuery(R"({{
        '$jsonSchema': {{
            'required': ['{0}.a']
        }}
    }})"),
                       AssertionException,
                       ErrorCodes::InvalidOptions);

    ASSERT_THROWS_CODE(_translateQuery(R"({{
        '$jsonSchema': {{
            'required': ['{0}', 0]
        }}
    }})"),
                       AssertionException,
                       ErrorCodes::InvalidOptions);
}

TEST_F(TimeseriesUpdateDeleteUtilTest, TranslateQueryJsonSchemaProperties) {
    _testTranslateQuery(R"({{
        '$jsonSchema': {{
            'required': ['{0}'],
            'properties': {{
                '{1}': {{
                    'bsonType': 'object',
                    'required': ['{2}']
                }}
            }}
        }}
    }})");

    ASSERT_THROWS_CODE(_translateQuery(R"({{
        '$jsonSchema': {{
            'required': ['{0}'],
            'properties': {{
                '{0}': {{
                    'bsonType': 'string'
                }},
                'notMetaField': {{
                    'bsonType': 'string'
                }}
            }}
        }}
    }})"),
                       AssertionException,
                       ErrorCodes::InvalidOptions);

    ASSERT_THROWS_CODE(_translateQuery(R"({{
        '$jsonSchema': {{
            'required': ['{0}'],
            'properties': {{
                '{0}.a': {{
                    'bsonType': 'string'
                }}
            }}
        }}
    }})"),
                       AssertionException,
                       ErrorCodes::InvalidOptions);

    ASSERT_THROWS_CODE(_translateQuery(R"({{
        '$jsonSchema': {{
            'required': ['{0}'],
            'properties': {{
                '0': {{
                    'bsonType': 'string'
                }}
            }}
        }}
    }})"),
                       AssertionException,
                       ErrorCodes::InvalidOptions);
}

TEST_F(TimeseriesUpdateDeleteUtilTest, TranslateQueryDisallowedOperators) {
    ASSERT_THROWS_CODE(_translateQuery(R"({{
            '$expr': {{
                '$eq': ['${0}', 0]
            }}
        }})"),
                       AssertionException,
                       ErrorCodes::InvalidOptions);

    ASSERT_THROWS_CODE(_translateQuery(R"({{
        '$where': 'function() {{ return true; }}'
    }})"),
                       AssertionException,
                       ErrorCodes::InvalidOptions);
}

TEST_F(TimeseriesUpdateDeleteUtilTest, TranslateUpdateEmpty) {
    ASSERT_THROWS_CODE(_translateUpdate(BSONObj{}), AssertionException, ErrorCodes::InvalidOptions);
}

TEST_F(TimeseriesUpdateDeleteUtilTest, TranslateUpdateSimple) {
    _testTranslateUpdate(R"({{
        '$set': {{'{1}': 'A'}}
    }})");
}

TEST_F(TimeseriesUpdateDeleteUtilTest, TranslateUpdateDotNotation) {
    _testTranslateUpdate(R"({{
        '$set': {{'{1}.a': 'A'}}
    }})");
}

TEST_F(TimeseriesUpdateDeleteUtilTest, TranslateUpdateMultiple) {
    _testTranslateUpdate(R"({{
        '$unset': {{'{0}.a': ''}},
        '$inc': {{'{1}.b': 10}}
    }})");
}

TEST_F(TimeseriesUpdateDeleteUtilTest, TranslateUpdatePrefix) {
    ASSERT_THROWS_CODE(_translateUpdate(R"({{
        '$unset': {{
            '{0}.a': ''
        }},
        '$inc': {{
            '{0}a': 10
        }}
    }})"),
                       AssertionException,
                       ErrorCodes::InvalidOptions);
}

TEST_F(TimeseriesUpdateDeleteUtilTest, TranslateUpdateRename) {
    _testTranslateUpdate(R"({{
        '$rename': {{'{0}.a': '{1}.b'}}
    }})");

    _testTranslateUpdate(R"({{
        '$rename': {{'{1}.{2}.{2}': '{0}.{2}.a'}}
    }})");

    _testTranslateUpdate(R"({{
        '$rename': {{'{0}.a.a': '{0}.b.b', '{1}.c': '{0}.d'}}
    }})");

    ASSERT_THROWS_CODE(_translateUpdate(R"({{
        '$rename': {{'{0}.a': 0 }}
    }})"),
                       AssertionException,
                       ErrorCodes::InvalidOptions);
}
}  // namespace
}  // namespace mongo
