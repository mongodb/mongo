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

#include "mongo/base/disallow_copying.h"
#include "mongo/base/status.h"
#include "mongo/logger/appender.h"
#include "mongo/logger/encoder.h"
#include "mongo/logger/rotatable_file_writer.h"

namespace mongo {
namespace logger {

    /**
     * Appender for writing to instances of RotatableFileWriter.
     */
    template <typename Event>
    class RotatableFileAppender : public Appender<Event> {
        MONGO_DISALLOW_COPYING(RotatableFileAppender);

    public:
        typedef Encoder<Event> EventEncoder;

        /**
         * Constructs an appender, that owns "encoder", but not "writer."  Caller must
         * keep "writer" in scope at least as long as the constructed appender.
         */
        RotatableFileAppender(EventEncoder* encoder, RotatableFileWriter* writer) :
            _encoder(encoder),
            _writer(writer) {
        }

        virtual Status append(const Event& event) {
            RotatableFileWriter::Use useWriter(_writer);
            Status status = useWriter.status();
            if (!status.isOK())
                return status;
            _encoder->encode(event, useWriter.stream()).flush();
            return useWriter.status();
        }

    private:
        boost::scoped_ptr<EventEncoder> _encoder;
        RotatableFileWriter* _writer;
    };

}  // namespace logger
}  // namespace mongo
