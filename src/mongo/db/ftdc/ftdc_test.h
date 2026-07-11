// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/util/modules.h"

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
