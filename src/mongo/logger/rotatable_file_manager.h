/*    Copyright 2013 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
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
         * Returns a vector of <filename, Status> pairs for filenames with non-OK rotate status.
         * An empty vector indicates that all files were rotated successfully.
         */
        FileNameStatusPairVector rotateAll(const std::string& renameTargetSuffix);

    private:
        typedef unordered_map<std::string, RotatableFileWriter*> WriterByNameMap;

        WriterByNameMap _writers;
    };

}  // namespace logger
}  // namespace mongo
