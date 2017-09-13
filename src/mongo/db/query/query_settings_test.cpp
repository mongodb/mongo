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

/**
 * This file contains tests for mongo/db/query/query_settings.h
 */

#include "mongo/db/query/query_settings.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/json.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/db/query/index_entry.h"
#include "mongo/unittest/unittest.h"

using mongo::AllowedIndicesFilter;
using mongo::BSONObj;
using mongo::IndexEntry;
using mongo::SimpleBSONObjComparator;
using mongo::fromjson;

namespace {
TEST(QuerySettingsTest, AllowedIndicesFilterAllowsIndexesByName) {
    SimpleBSONObjComparator bsonCmp;
    AllowedIndicesFilter filter(bsonCmp.makeBSONObjSet({fromjson("{a:1}")}), {"a_1"});
    IndexEntry a_idx(fromjson("{a:1, b:1}"), false, false, false, "a_1", nullptr, BSONObj());
    IndexEntry ab_idx(fromjson("{a:1, b:1}"), false, false, false, "a_1:2", nullptr, BSONObj());

    ASSERT_TRUE(filter.allows(a_idx));
    ASSERT_FALSE(filter.allows(ab_idx));
}

TEST(QuerySettingsTest, AllowedIndicesFilterAllowsIndexesByKeyPattern) {
    SimpleBSONObjComparator bsonCmp;
    AllowedIndicesFilter filter(bsonCmp.makeBSONObjSet({fromjson("{a:1}")}), {"a"});
    IndexEntry a_idx(fromjson("{a:1}"), false, false, false, "foo", nullptr, BSONObj());
    IndexEntry ab_idx(fromjson("{a:1, b:1}"), false, false, false, "bar", nullptr, BSONObj());

    ASSERT_TRUE(filter.allows(a_idx));
    ASSERT_FALSE(filter.allows(ab_idx));
}
}
