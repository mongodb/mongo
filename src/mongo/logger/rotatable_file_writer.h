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

#include <boost/scoped_ptr.hpp>
#include <boost/thread/mutex.hpp>
#include <iostream>
#include <string>

#include "mongo/base/disallow_copying.h"
#include "mongo/base/status.h"

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
             * Returns Status::OK() on success.  If the rename fails, returns
             * ErrorCodes::FileRenameFailed, and the stream continues to write to the unrotated
             * file.  If the rename succeeds but the subsequent file open fails, returns
             * ErrorCodes::FileNotOpen, and the stream continues to target the original file, though
             * under its new name.
             */
            Status rotate(const std::string& renameTarget);

            /**
             * Returns the status of the stream.
             *
             * One of Status::OK(), ErrorCodes::FileNotOpen and ErrorCodes::FileStreamFailed.
             */
            Status status();

            /**
             * Returns a reference to the std::ostream() through which users may write to the file.
             */
            std::ostream& stream() { return *_writer->_stream; }

        private:
            /**
             * Helper that opens the file named by setFileName(), in the mode specified by "append".
             *
             * Returns Status::OK() on success and ErrorCodes::FileNotOpen on failure.
             */
            Status _openFileStream(bool append);

            RotatableFileWriter* _writer;
            boost::unique_lock<boost::mutex> _lock;
        };

        /**
         * Constructs an instance of RotatableFileWriter.
         */
        RotatableFileWriter();

    private:
        friend class RotatableFileWriter::Use;
        boost::mutex _mutex;
        std::string _fileName;
        boost::scoped_ptr<std::ostream> _stream;
    };

}  // namespace logger
}  // namespace mongo
