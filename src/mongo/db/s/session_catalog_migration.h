// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/util/modules.h"

#include <string_view>

namespace mongo {
using namespace std::literals::string_view_literals;
class SessionCatalogMigration {
public:
    static constexpr std::string_view kSessionMigrateOplogTag = "$sessionMigrateInfo"sv;
    static const BSONObj kSessionOplogTag;
};
}  // namespace mongo
