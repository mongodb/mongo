// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

// IWYU pragma: private, include "mongo/db/database_name.h"

/**
 * This X-macro expands the provided macro as `X(id, db)` for each dbname:
 *
 * - `id` : the `ConstantProxy` data member of `DatabaseName` being defined.
 * - `db` : a constexpr std::string_view expression.
 */

#define EXPAND_DBNAME_CONSTANT_TABLE(X) \
    X(kAdmin, "admin")                  \
    X(kLocal, "local")                  \
    X(kConfig, "config")                \
    X(kSystem, "system")                \
    X(kExternal, "$external")           \
    X(kEmpty, "")                       \
    X(kMdbTesting, "mdb_testing")       \
    X(kGlobal, "global")                \
    X(kMdbCatalog, "_mdb_catalog")      \
    /**/
