/*    Copyright 2013 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#pragma once

#include <string>
#include <utility>
#include <vector>

#include "mongo/base/disallow_copying.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/logger/rotatable_file_writer.h"
#include "mongo/platform/unordered_map.h"

namespace mongo {
namespace logger {

typedef StatusWith<RotatableFileWriter*> StatusWithRotatableFileWriter;

/**
 * Utility object that owns and manages rotation for RotatableFileWriters.
 *
 * Unlike RotatableFileWriter, this type leaves synchronization to its consumers.
 */
class RotatableFileManager {
    MONGO_DISALLOW_COPYING(RotatableFileManager);

public:
    typedef std::pair<std::string, Status> FileNameStatusPair;
    typedef std::vector<FileNameStatusPair> FileNameStatusPairVector;

    RotatableFileManager();
    ~RotatableFileManager();

    /**
     * Opens "name" in mode "append" and returns a new RotatableFileWriter set to
     * operate on the file.
     *
     * If the manager already has opened "name", returns ErrorCodes::FileAlreadyOpen.
     * May also return failure codes issued by RotatableFileWriter::Use::setFileName().
     */
    StatusWithRotatableFileWriter openFile(const std::string& name, bool append);

    /**
     * Gets a RotatableFileWriter for writing to "name", if the manager owns one, or NULL if
     * not.
     */
    RotatableFileWriter* getFile(const std::string& name);

    /**
     * Rotates all managed files, renaming each file by appending "renameTargetSuffix".
     *
     * renameFiles - true we rename the log file, false we expect it was renamed externally
     *
     * Returns a vector of <filename, Status> pairs for filenames with non-OK rotate status.
     * An empty vector indicates that all files were rotated successfully.
     */
    FileNameStatusPairVector rotateAll(bool renameFiles, const std::string& renameTargetSuffix);

private:
    typedef unordered_map<std::string, RotatableFileWriter*> WriterByNameMap;

    WriterByNameMap _writers;
};

}  // namespace logger
}  // namespace mongo
