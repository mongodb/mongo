/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#define CATCH_CONFIG_RUNNER
#include <catch2/catch.hpp>

#include "utils.h"

int
main(int argc, char **argv)
{
    // Clean up after any previous failed/crashed test runs.
    utils::wiredtigerCleanup(DB_HOME);

    return Catch::Session().run(argc, argv);
}
