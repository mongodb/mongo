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

/**
 * This file contains tests for mongo/db/query/query_settings.h
 */

#include "mongo/db/query/query_settings.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/json.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/db/index_names.h"
#include "mongo/db/query/index_entry.h"
#include "mongo/unittest/unittest.h"

using mongo::AllowedIndicesFilter;
using mongo::BSONObj;
using mongo::fromjson;
using mongo::IndexEntry;
using mongo::SimpleBSONObjComparator;

namespace {
TEST(QuerySettingsTest, AllowedIndicesFilterAllowsIndexesByName) {
    SimpleBSONObjComparator bsonCmp;
    AllowedIndicesFilter filter(bsonCmp.makeBSONObjSet({fromjson("{a:1}")}), {"a_1"});
    auto keyPat = fromjson("{a:1, b:1}");
    IndexEntry a_idx(keyPat,
                     mongo::IndexNames::nameToType(mongo::IndexNames::findPluginName(keyPat)),
                     false,
                     {},
                     {},
                     false,
                     false,
                     IndexEntry::Identifier{"a_1"},
                     nullptr,
                     BSONObj(),
                     nullptr,
                     nullptr);
    IndexEntry ab_idx(keyPat,
                      mongo::IndexNames::nameToType(mongo::IndexNames::findPluginName(keyPat)),
                      false,
                      {},
                      {},
                      false,
                      false,
                      IndexEntry::Identifier{"a_1:2"},
                      nullptr,
                      BSONObj(),
                      nullptr,
                      nullptr);

    ASSERT_TRUE(filter.allows(a_idx));
    ASSERT_FALSE(filter.allows(ab_idx));
}

TEST(QuerySettingsTest, AllowedIndicesFilterAllowsIndexesByKeyPattern) {
    SimpleBSONObjComparator bsonCmp;
    AllowedIndicesFilter filter(bsonCmp.makeBSONObjSet({fromjson("{a:1}")}), {"a"});
    auto keyPat_a = fromjson("{a:1}");
    IndexEntry a_idx(keyPat_a,
                     mongo::IndexNames::nameToType(mongo::IndexNames::findPluginName(keyPat_a)),
                     false,
                     {},
                     {},
                     false,
                     false,
                     IndexEntry::Identifier{"foo"},
                     nullptr,
                     BSONObj(),
                     nullptr,
                     nullptr);
    auto keyPat_ab = fromjson("{a:1, b:1}");
    IndexEntry ab_idx(keyPat_ab,
                      mongo::IndexNames::nameToType(mongo::IndexNames::findPluginName(keyPat_ab)),
                      false,
                      {},
                      {},
                      false,
                      false,
                      IndexEntry::Identifier{"bar"},
                      nullptr,
                      BSONObj(),
                      nullptr,
                      nullptr);

    ASSERT_TRUE(filter.allows(a_idx));
    ASSERT_FALSE(filter.allows(ab_idx));
}
}  // namespace
