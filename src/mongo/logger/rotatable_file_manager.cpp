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
    std::unique_ptr<RotatableFileWriter> writer(new RotatableFileWriter);
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
    bool renameFiles, const std::string& renameTargetSuffix) {
    FileNameStatusPairVector badStatuses;
    for (WriterByNameMap::const_iterator iter = _writers.begin(); iter != _writers.end(); ++iter) {
        Status status = RotatableFileWriter::Use(iter->second)
                            .rotate(renameFiles, iter->first + renameTargetSuffix);
        if (!status.isOK()) {
            badStatuses.push_back(std::make_pair(iter->first, status));
        }
    }
    return badStatuses;
}

}  // namespace logger
}  // namespace mongo
