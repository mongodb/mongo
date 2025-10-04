/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/bson/bsonobj.h"
#include "mongo/db/service_context_test_fixture.h"

#include <vector>

#include <boost/filesystem.hpp>
#include <boost/filesystem/path.hpp>

namespace mongo {

class FTDCTest : public ClockSourceMockServiceContextTest {};

/**
 * Validation mode for tests, strict by default
 */
enum class FTDCValidationMode {
    /**
     * Compare BSONObjs exactly.
     */
    kStrict,

    /**
     * Compare BSONObjs by only comparing types FTDC compares about. FTDC ignores somes changes in
     * the shapes of documents and therefore no longer reconstructs the shapes of documents exactly.
     */
    kWeak,
};

/**
 * Validate the documents in a file match the specified vector.
 *
 * Unit Test ASSERTs if there is mismatch.
 */
void ValidateDocumentList(const boost::filesystem::path& path,
                          const std::vector<BSONObj>& docs,
                          FTDCValidationMode mode);

/**
 * Validate that two lists of documents are equal.
 *
 * Unit Test ASSERTs if there is mismatch.
 */
void ValidateDocumentList(const std::vector<BSONObj>& docs1,
                          const std::vector<BSONObj>& docs2,
                          FTDCValidationMode mode);

/**
 * Validate the documents in a file matches the documents in the specified vectors, which
 * are sorted by type.
 *
 * Unit Test ASSERTs if there is mismatch.
 */
void ValidateDocumentListByType(const std::vector<boost::filesystem::path>& paths,
                                const std::vector<BSONObj>& expectedOnRotateMetadata,
                                const std::vector<BSONObj>& expectedMetrics,
                                const std::vector<BSONObj>& expectedPeriodicMetadata,
                                FTDCValidationMode mode);

/**
 * Delete a file if it exists.
 */
void deleteFileIfNeeded(const boost::filesystem::path& p);

/**
 * Get a list of files in a directory.
 */
std::vector<boost::filesystem::path> scanDirectory(const boost::filesystem::path& path);

/**
 * Create a new directory, and ensure it is empty.
 */
void createDirectoryClean(const boost::filesystem::path& dir);

}  // namespace mongo
