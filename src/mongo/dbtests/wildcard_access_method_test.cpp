// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/json.h"
#include "mongo/db/field_ref.h"
#include "mongo/db/query/compiler/physical_model/index_bounds/index_bounds.h"
#include "mongo/db/query/compiler/physical_model/interval/interval.h"
#include "mongo/db/query/query_planner_test_lib.h"
#include "mongo/db/query/wildcard_multikey_paths.h"
#include "mongo/unittest/unittest.h"

#include <string_view>
#include <utility>
#include <vector>

namespace mongo {
namespace {

void assertCorrectMultikeyMetadataPathBoundsGenerated(std::string_view field,
                                                      const BSONObj& expectedBounds) {
    IndexBounds indexBounds;

    OrderedIntervalList orderedIntervalList;
    FieldRef fieldRef(field);
    orderedIntervalList.intervals = getMultikeyPathIndexIntervalsForField(std::move(fieldRef));

    indexBounds.fields.push_back(std::move(orderedIntervalList));

    const bool kDoNotRelaxBoundsCheck = false;
    ASSERT(QueryPlannerTestLib::boundsMatch(
               expectedBounds, std::move(indexBounds), kDoNotRelaxBoundsCheck)
               .isOK());
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
