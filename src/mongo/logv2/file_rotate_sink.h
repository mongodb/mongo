/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include <boost/log/sinks/text_ostream_backend.hpp>
#include <memory>
#include <string>

#include "mongo/base/status.h"
#include "mongo/logv2/log_format.h"

namespace mongo::logv2 {
// boost::log backend sink to provide MongoDB style file rotation.
// Uses custom stream type to open log files with shared access on Windows, somthing the built-in
// boost file rotation sink does not do.
class FileRotateSink : public boost::log::sinks::text_ostream_backend {
public:
    FileRotateSink(LogTimestampFormat timestampFormat);
    ~FileRotateSink();

    Status addFile(const std::string& filename, bool append);
    void removeFile(const std::string& filename);

    Status rotate(bool rename, StringData renameSuffix);

    void consume(const boost::log::record_view& rec, const string_type& formatted_string);

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

}  // namespace mongo::logv2
