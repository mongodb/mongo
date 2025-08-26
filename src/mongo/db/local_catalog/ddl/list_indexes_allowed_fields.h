/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#pragma once

#include "mongo/base/string_data.h"
#include "mongo/db/index_names.h"
#include "mongo/db/local_catalog/ddl/list_indexes_gen.h"

#include <map>

namespace mongo {
// The allowed fields have to be in sync with those defined in 'src/mongo/db/list_indexes.idl'.
inline static std::map<StringData, std::set<IndexType>> kAllowedListIndexesFieldNames = {
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
