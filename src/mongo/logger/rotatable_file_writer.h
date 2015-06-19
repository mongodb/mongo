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

#include <memory>
#include <ostream>
#include <string>

#include "mongo/base/disallow_copying.h"
#include "mongo/base/status.h"
#include "mongo/stdx/mutex.h"

namespace mongo {
namespace logger {

/**
 * A synchronized file output stream writer, with support for file rotation.
 *
 * To enforce proper locking, instances of RotatableFileWriter may only be manipulated by
 * instantiating a RotatableFileWriter::Use guard object, which exposes the relevant
 * manipulation methods for the stream.  For any instance of RotatableFileWriter, at most one
 * fully constructed instance of RotatableFileWriter::Use exists at a time, providing mutual
 * exclusion.
 *
 * Behavior is undefined if two instances of RotatableFileWriter should simultaneously have the
 * same value for their fileName.
 */
class RotatableFileWriter {
    MONGO_DISALLOW_COPYING(RotatableFileWriter);

public:
    /**
     * Guard class representing synchronous use of an instance of RotatableFileWriter.
     */
    class Use {
        MONGO_DISALLOW_COPYING(Use);

    public:
        /**
         * Constructs a Use object for "writer", and lock "writer".
         */
        explicit Use(RotatableFileWriter* writer);

        /**
         * Sets the name of the target file to which stream() writes to "name".
         *
         * May be called repeatedly.
         *
         * If this method does not return Status::OK(), it is not safe to call rotate() or
         * stream().
         *
         * Set "append" to true to open "name" in append mode.  Otherwise, it is truncated.
         */
        Status setFileName(const std::string& name, bool append);

        /**
         * Rotates the currently opened file into "renameTarget", and open a new file
         * with the name previously set via setFileName().
         *
         * renameFiles - true we rename the log file, false we expect it was renamed externally
         *
         * Returns Status::OK() on success.  If the rename fails, returns
         * ErrorCodes::FileRenameFailed, and the stream continues to write to the unrotated
         * file.  If the rename succeeds but the subsequent file open fails, returns
         * ErrorCodes::FileNotOpen, and the stream continues to target the original file, though
         * under its new name.
         */
        Status rotate(bool renameFile, const std::string& renameTarget);

        /**
         * Returns the status of the stream.
         *
         * One of Status::OK(), ErrorCodes::FileNotOpen and ErrorCodes::FileStreamFailed.
         */
        Status status();

        /**
         * Returns a reference to the std::ostream() through which users may write to the file.
         */
        std::ostream& stream() {
            return *_writer->_stream;
        }

    private:
        /**
         * Helper that opens the file named by setFileName(), in the mode specified by "append".
         *
         * Returns Status::OK() on success and ErrorCodes::FileNotOpen on failure.
         */
        Status _openFileStream(bool append);

        RotatableFileWriter* _writer;
        stdx::unique_lock<stdx::mutex> _lock;
    };

    /**
     * Constructs an instance of RotatableFileWriter.
     */
    RotatableFileWriter();

private:
    friend class RotatableFileWriter::Use;
    stdx::mutex _mutex;
    std::string _fileName;
    std::unique_ptr<std::ostream> _stream;
};

}  // namespace logger
}  // namespace mongo
