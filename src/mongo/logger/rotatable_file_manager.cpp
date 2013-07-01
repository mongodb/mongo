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

#include "mongo/platform/basic.h"

#include "mongo/logger/rotatable_file_manager.h"

#include "mongo/logger/rotatable_file_writer.h"
#include "mongo/util/map_util.h"

namespace mongo {
namespace logger {

    RotatableFileManager::RotatableFileManager() {}

    RotatableFileManager::~RotatableFileManager() {
        for (WriterByNameMap::iterator iter = _writers.begin(); iter != _writers.end(); ++iter) {
            delete iter->second;
        }
    }

    StatusWithRotatableFileWriter RotatableFileManager::openFile(const std::string& fileName,
                                                                 bool append) {
        if (_writers.count(fileName) > 0) {
            return StatusWithRotatableFileWriter(ErrorCodes::FileAlreadyOpen,
                                                 "File \"" + fileName + "\" already opened.");
        }
        std::auto_ptr<RotatableFileWriter> writer(new RotatableFileWriter);
        RotatableFileWriter::Use writerUse(writer.get());
        Status status = writerUse.setFileName(fileName, append);
        if (!status.isOK())
            return StatusWithRotatableFileWriter(status);
        _writers.insert(std::make_pair(fileName, writer.get()));
        return StatusWith<RotatableFileWriter*>(writer.release());
    }

    RotatableFileWriter* RotatableFileManager::getFile(const std::string& name) {
        return mapFindWithDefault(_writers, name, static_cast<RotatableFileWriter*>(NULL));
    }

    RotatableFileManager::FileNameStatusPairVector RotatableFileManager::rotateAll(
            const std::string& renameTargetSuffix) {

        FileNameStatusPairVector badStatuses;
        for (WriterByNameMap::const_iterator iter = _writers.begin();
             iter != _writers.end(); ++iter) {

            Status status = RotatableFileWriter::Use(iter->second).rotate(
                    iter->first + renameTargetSuffix);
            if (!status.isOK()) {
                badStatuses.push_back(std::make_pair(iter->first, status));
            }
        }
        return badStatuses;
    }

}  // namespace logger
}  // namespace mongo
