// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/query/query_settings_decoration.h"

#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/util/decorable.h"


#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery


namespace mongo {

namespace {

const auto getQuerySettingsDecoration =
    SharedCollectionDecorations::declareDecoration<QuerySettingsDecoration>();

}  // namespace

QuerySettings* QuerySettingsDecoration::get(SharedCollectionDecorations* decorations) {
    return getQuerySettingsDecoration(decorations)._querySettings.get();
}

QuerySettingsDecoration::QuerySettingsDecoration()
    : _querySettings(std::make_unique<QuerySettings>()) {};

}  // namespace mongo
