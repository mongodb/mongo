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

#include "mongo/bson/json.h"
#include "mongo/db/index/wildcard_access_method.h"
#include "mongo/db/query/query_planner_test_lib.h"
#include "mongo/db/query/wildcard_multikey_paths.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

void assertCorrectMultikeyMetadataPathBoundsGenerated(StringData field,
                                                      const BSONObj& expectedBounds) {
    IndexBounds indexBounds;

    OrderedIntervalList orderedIntervalList;
    FieldRef fieldRef(field);
    orderedIntervalList.intervals = getMultikeyPathIndexIntervalsForField(std::move(fieldRef));

    indexBounds.fields.push_back(std::move(orderedIntervalList));

    const bool kDoNotRelaxBoundsCheck = false;
    ASSERT(QueryPlannerTestLib::boundsMatch(
        expectedBounds, std::move(indexBounds), kDoNotRelaxBoundsCheck));
}

TEST(WildcardAccessMethodTest, FieldGeneratesExpectedMultikeyPathBounds) {
    assertCorrectMultikeyMetadataPathBoundsGenerated("foo",
                                                     fromjson("{'': [['foo','foo',true,true]]}"));
    assertCorrectMultikeyMetadataPathBoundsGenerated(
        "foo.bar", fromjson("{'': [['foo','foo',true,true], ['foo.bar','foo.bar',true,true]]}"));
    assertCorrectMultikeyMetadataPathBoundsGenerated(
        "foo.0", fromjson("{'': [['foo','foo',true,true], ['foo.','foo/',true,false]]}"));
    assertCorrectMultikeyMetadataPathBoundsGenerated(
        "foo.0.bar", fromjson("{'': [['foo','foo',true,true], ['foo.','foo/',true,false]]}"));
    assertCorrectMultikeyMetadataPathBoundsGenerated(
        "foo.bar.0",
        fromjson("{'': [['foo','foo',true,true], ['foo.bar','foo.bar',true,true], "
                 "['foo.bar.','foo.bar/',true,false]]}"));
    assertCorrectMultikeyMetadataPathBoundsGenerated(
        "foo.0.bar.1", fromjson("{'': [['foo','foo',true,true], ['foo.','foo/',true,false]]}"));
}

}  // namespace
}  // namespace mongo
