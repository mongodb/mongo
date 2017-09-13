/**
 * Copyright (C) 2015 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#include <boost/filesystem.hpp>
#include <vector>

#include "mongo/db/jsobj.h"

namespace mongo {

/**
 * Validate the documents in a file match the specified vector.
 *
 * Unit Test ASSERTs if there is mismatch.
 */
void ValidateDocumentList(const boost::filesystem::path& p, const std::vector<BSONObj>& docs);

/**
 * Validate that two lists of documents are equal.
 *
 * Unit Test ASSERTs if there is mismatch.
 */
void ValidateDocumentList(const std::vector<BSONObj>& docs1, const std::vector<BSONObj>& docs2);

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
