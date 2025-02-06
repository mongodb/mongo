/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/namespace_string.h"

// The logging facility enforces the rule that logging should not be done in a header file. Since
// template classes and functions below must be defined in the header file and since they use the
// logging facility, we have to define the helper functions below to perform the actual logging
// operation from template code.
namespace mongo::log_detail {
void logInactiveCacheEntry(const std::string& key);
void logCacheEviction(NamespaceString nss, std::string&& evictedEntry);
void logCreateInactiveCacheEntry(std::string&& query,
                                 std::string&& planCacheShapeHash,
                                 std::string&& planCacheKey,
                                 size_t newWorks);
void logReplaceActiveCacheEntry(std::string&& query,
                                std::string&& planCacheShapeHash,
                                std::string&& planCacheKey,
                                size_t works,
                                size_t newWorks);
void logNoop(std::string&& query,
             std::string&& planCacheShapeHash,
             std::string&& planCacheKey,
             size_t works,
             size_t newWorks);
void logIncreasingWorkValue(std::string&& query,
                            std::string&& planCacheShapeHash,
                            std::string&& planCacheKey,
                            size_t works,
                            size_t increasedWorks);
void logPromoteCacheEntry(std::string&& query,
                          std::string&& planCacheShapeHash,
                          std::string&& planCacheKey,
                          size_t works,
                          size_t newWorks);
void logUnexpectedPinnedCacheEntry(std::string&& query,
                                   std::string&& planCacheShapeHash,
                                   std::string&& planCacheKey,
                                   std::string&& oldEntry,
                                   std::string&& newEntry,
                                   std::string&& oldSbePlan,
                                   std::string&& newSbePlan,
                                   size_t newWorks);
}  // namespace mongo::log_detail
