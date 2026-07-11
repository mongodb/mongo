// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

/**
 * This file contains tests for mongo/db/query/query_settings.h
 */

#include "mongo/db/query/query_settings.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/json.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/db/exec/mutable_bson/mutable_bson_test_utils.h"
#include "mongo/db/index_names.h"
#include "mongo/db/query/compiler/metadata/index_entry.h"
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
                     mongo::IndexConfig::kLatestIndexVersion,
                     false,
                     {},
                     {},
                     false,
                     false,
                     IndexEntry::Identifier{"a_1"},
                     BSONObj(),
                     nullptr);
    IndexEntry ab_idx(keyPat,
                      mongo::IndexNames::nameToType(mongo::IndexNames::findPluginName(keyPat)),
                      mongo::IndexConfig::kLatestIndexVersion,
                      false,
                      {},
                      {},
                      false,
                      false,
                      IndexEntry::Identifier{"a_1:2"},
                      BSONObj(),
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
                     mongo::IndexConfig::kLatestIndexVersion,
                     false,
                     {},
                     {},
                     false,
                     false,
                     IndexEntry::Identifier{"foo"},
                     BSONObj(),
                     nullptr);
    auto keyPat_ab = fromjson("{a:1, b:1}");
    IndexEntry ab_idx(keyPat_ab,
                      mongo::IndexNames::nameToType(mongo::IndexNames::findPluginName(keyPat_ab)),
                      mongo::IndexConfig::kLatestIndexVersion,
                      false,
                      {},
                      {},
                      false,
                      false,
                      IndexEntry::Identifier{"bar"},
                      BSONObj(),
                      nullptr);

    ASSERT_TRUE(filter.allows(a_idx));
    ASSERT_FALSE(filter.allows(ab_idx));
}
}  // namespace
