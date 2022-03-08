/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */
#include <string>

#define DB_HOME "test_db"

namespace utils {
void throwIfNonZero(int result);
void wiredtigerCleanup(const std::string &db_home);
} // namespace utils
