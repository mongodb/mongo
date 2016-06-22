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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/client/index_spec.h"

#include "mongo/unittest/unittest.h"

#define ASSERT_UASSERTS(STATEMENT) ASSERT_THROWS(STATEMENT, UserException)

namespace mongo {

TEST(Options, RepeatedOptionsFail) {
    ASSERT_UASSERTS(IndexSpec().background().background());
    ASSERT_UASSERTS(IndexSpec().unique().unique());
    ASSERT_UASSERTS(IndexSpec().dropDuplicates().dropDuplicates());
    ASSERT_UASSERTS(IndexSpec().sparse().sparse());
    ASSERT_UASSERTS(IndexSpec().expireAfterSeconds(1).expireAfterSeconds(1));
    ASSERT_UASSERTS(IndexSpec().version(0).version(0));
    ASSERT_UASSERTS(IndexSpec().textWeights(BSONObj()).textWeights(BSONObj()));
    ASSERT_UASSERTS(IndexSpec().textDefaultLanguage("foo").textDefaultLanguage("foo"));
    ASSERT_UASSERTS(IndexSpec().textLanguageOverride("foo").textLanguageOverride("foo"));
    ASSERT_UASSERTS(IndexSpec().textIndexVersion(0).textIndexVersion(0));
    ASSERT_UASSERTS(IndexSpec().geo2DSphereIndexVersion(0).geo2DSphereIndexVersion(0));
    ASSERT_UASSERTS(IndexSpec().geo2DBits(0).geo2DBits(0));
    ASSERT_UASSERTS(IndexSpec().geo2DMin(2.00).geo2DMin(2.00));
    ASSERT_UASSERTS(IndexSpec().geo2DMax(2.00).geo2DMax(2.00));
    ASSERT_UASSERTS(IndexSpec().geoHaystackBucketSize(2.0).geoHaystackBucketSize(2.0));
    ASSERT_UASSERTS(IndexSpec().addOptions(BSON("foo" << 1 << "foo" << 1)));
    ASSERT_UASSERTS(IndexSpec().sparse(0).addOptions(BSON("sparse" << 1)));
}

TEST(Options, RepeatedKeysFail) {

    IndexSpec spec;
    spec.addKey("aField");

    ASSERT_UASSERTS(spec.addKey("aField"));

    const BSONObj fields = BSON("someField" << 1 << "aField" << 1 << "anotherField" << 1);
    ASSERT_UASSERTS(spec.addKey(fields.getField("aField")));
    ASSERT_UASSERTS(spec.addKeys(fields));
}

TEST(Options, NameIsHonored) {
    IndexSpec spec;
    spec.addKey("aField");

    // Should get an auto generated name
    ASSERT_FALSE(spec.name().empty());

    // That is not the name we are about to set.
    ASSERT_NE("someName", spec.name());

    spec.name("someName");

    // Should get the name we specified.
    ASSERT_EQ("someName", spec.name());

    // Name can be changed as many times as we want
    spec.name("yetAnotherName");
    ASSERT_EQ("yetAnotherName", spec.name());
}

}  // namespace mongo
