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
    RotatableFileAppender(const RotatableFileAppender&) = delete;
    RotatableFileAppender& operator=(const RotatableFileAppender&) = delete;

public:
    typedef Encoder<Event> EventEncoder;

    // TODO: Remove this ctor once raw pointer use is gone
    RotatableFileAppender(EventEncoder* encoder, RotatableFileWriter* writer)
        : _encoder(encoder), _writer(writer) {}
    /**
     * Constructs an appender, that owns "encoder", but not "writer."  Caller must
     * keep "writer" in scope at least as long as the constructed appender.
     */
    RotatableFileAppender(std::unique_ptr<EventEncoder> encoder, RotatableFileWriter* writer)
        : _encoder(std::move(encoder)), _writer(writer) {}

    virtual Status append(const Event& event) {
        RotatableFileWriter::Use useWriter(_writer);
        Status status = useWriter.status();
        if (!status.isOK())
            return status;
        _encoder->encode(event, useWriter.stream()).flush();
        return useWriter.status();
    }

private:
    std::unique_ptr<EventEncoder> _encoder;
    RotatableFileWriter* _writer;
};

}  // namespace logger
}  // namespace mongo
