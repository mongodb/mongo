// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/index_names.h"
#include "mongo/db/shard_role/ddl/list_indexes_gen.h"
#include "mongo/util/modules.h"

#include <map>
#include <string_view>

namespace mongo {

// The allowed fields have to be in sync with those defined in 'src/mongo/db/list_indexes.idl'.
[[MONGO_MOD_PUBLIC]] inline static std::map<std::string_view, std::set<IndexType>>
    kAllowedListIndexesFieldNames = {
        {ListIndexesReplyItem::k2dsphereIndexVersionFieldName,
         {IndexType::INDEX_2DSPHERE, IndexType::INDEX_2DSPHERE_BUCKET}},
        {ListIndexesReplyItem::kBackgroundFieldName, {}},
        {ListIndexesReplyItem::kBitsFieldName, {IndexType::INDEX_2D}},
        {ListIndexesReplyItem::kBucketSizeFieldName, {}},
        {ListIndexesReplyItem::kBuildUUIDFieldName, {}},
        {ListIndexesReplyItem::kClusteredFieldName, {}},
        {ListIndexesReplyItem::kCoarsestIndexedLevelFieldName, {IndexType::INDEX_2DSPHERE}},
        {ListIndexesReplyItem::kCollationFieldName, {}},
        {ListIndexesReplyItem::kDefault_languageFieldName, {}},
        {ListIndexesReplyItem::kDropDupsFieldName, {}},
        {ListIndexesReplyItem::kExpireAfterSecondsFieldName, {}},
        {ListIndexesReplyItem::kFinestIndexedLevelFieldName, {IndexType::INDEX_2DSPHERE}},
        {ListIndexesReplyItem::kHiddenFieldName, {}},
        {ListIndexesReplyItem::kIndexBuildInfoFieldName, {}},
        {ListIndexesReplyItem::kKeyFieldName, {}},
        {ListIndexesReplyItem::kLanguage_overrideFieldName, {}},
        {ListIndexesReplyItem::kMaxFieldName, {IndexType::INDEX_2D}},
        {ListIndexesReplyItem::kMinFieldName, {IndexType::INDEX_2D}},
        {ListIndexesReplyItem::kNameFieldName, {}},
        {ListIndexesReplyItem::kNsFieldName, {}},
        {ListIndexesReplyItem::kOriginalSpecFieldName, {}},
        {ListIndexesReplyItem::kPartialFilterExpressionFieldName, {}},
        {ListIndexesReplyItem::kPrepareUniqueFieldName, {}},
        {ListIndexesReplyItem::kSparseFieldName, {}},
        {ListIndexesReplyItem::kSpecFieldName, {}},
        {ListIndexesReplyItem::kStorageEngineFieldName, {}},
        {ListIndexesReplyItem::kTextIndexVersionFieldName, {IndexType::INDEX_TEXT}},
        {ListIndexesReplyItem::kUniqueFieldName, {}},
        {ListIndexesReplyItem::kVFieldName, {}},
        {ListIndexesReplyItem::kWeightsFieldName, {IndexType::INDEX_TEXT}},
        {ListIndexesReplyItem::kWildcardProjectionFieldName, {IndexType::INDEX_WILDCARD}},
};

}  // namespace mongo
