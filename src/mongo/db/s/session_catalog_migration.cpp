// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/s/session_catalog_migration.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
namespace mongo {

const BSONObj SessionCatalogMigration::kSessionOplogTag(
    BSON(SessionCatalogMigration::kSessionMigrateOplogTag << 1));

}  // namespace mongo
