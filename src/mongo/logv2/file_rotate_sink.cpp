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

#include "mongo/logv2/file_rotate_sink.h"

#include <boost/exception/diagnostic_information.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/iterator/filter_iterator.hpp>
#include <boost/iterator/transform_iterator.hpp>
#include <boost/make_shared.hpp>
#include <fmt/format.h>
#include <fstream>

#include "mongo/logv2/json_formatter.h"
#include "mongo/logv2/log_detail.h"
#include "mongo/logv2/shared_access_fstream.h"
#include "mongo/util/exit_code.h"
#include "mongo/util/quick_exit.h"
#include "mongo/util/stacktrace.h"
#include "mongo/util/string_map.h"

namespace mongo::logv2 {
namespace {

using namespace fmt::literals;

#if _WIN32
using stream_t = Win32SharedAccessOfstream;
#else
using stream_t = std::ofstream;
#endif

StatusWith<boost::shared_ptr<stream_t>> openFile(const std::string& filename, bool append) {
    std::ios_base::openmode mode = std::ios_base::out;
    bool exists = false;
    if (append) {
        mode |= std::ios_base::app;
        exists = boost::filesystem::exists(filename);
    } else
        mode |= std::ios_base::trunc;
    auto file = boost::make_shared<stream_t>(filename, mode);
    if (file->fail())
        return Status(ErrorCodes::FileNotOpen, fmt::format("Failed to open {}", filename));
    if (append && exists)
        file->put('\n');
    return file;
}
}  // namespace

struct FileRotateSink::Impl {
    Impl(LogTimestampFormat tsFormat) : timestampFormat(tsFormat) {}
    StringMap<boost::shared_ptr<stream_t>> files;
    LogTimestampFormat timestampFormat;
};

FileRotateSink::FileRotateSink(LogTimestampFormat timestampFormat)
    : _impl(std::make_unique<Impl>(timestampFormat)) {}
FileRotateSink::~FileRotateSink() {}

Status FileRotateSink::addFile(const std::string& filename, bool append) {
    auto statusWithFile = openFile(filename, append);
    if (statusWithFile.isOK()) {
        add_stream(statusWithFile.getValue());
        _impl->files[filename] = statusWithFile.getValue();
    }

    return statusWithFile.getStatus().withContext("Can't initialize rotatable log file");
}
void FileRotateSink::removeFile(const std::string& filename) {
    auto it = _impl->files.find(filename);
    if (it != _impl->files.cend()) {
        remove_stream(it->second);
        _impl->files.erase(it);
    }
}

Status FileRotateSink::rotate(bool rename,
                              StringData renameSuffix,
                              std::function<void(Status)> onMinorError) {
    for (auto& file : _impl->files) {
        const std::string& filename = file.first;
        if (rename) {
            std::string renameTarget = filename + renameSuffix;

            auto targetExists = [&]() -> StatusWith<bool> {
                try {
                    return boost::filesystem::exists(renameTarget);
                } catch (const boost::exception&) {
                    return exceptionToStatus();
                }
            }();

            if (!targetExists.isOK()) {
                return Status(ErrorCodes::FileRenameFailed, targetExists.getStatus().reason())
                    .withContext("Cannot verify whether destination already exists: {}"_format(
                        renameTarget));
            }

            if (targetExists.getValue()) {
                if (onMinorError) {
                    onMinorError({ErrorCodes::FileRenameFailed,
                                  "Target already exists during log rotation. Skipping this file. "
                                  "target={}, file={}"_format(renameTarget, filename)});
                }
                continue;
            }

            boost::system::error_code ec;
            boost::filesystem::rename(filename, renameTarget, ec);
            if (ec) {
                if (ec == boost::system::errc::no_such_file_or_directory) {
                    if (onMinorError)
                        onMinorError(
                            {ErrorCodes::FileRenameFailed,
                             "Source file was missing during log rotation. Creating a new one. "
                             "file={}"_format(filename)});
                } else {
                    return Status(ErrorCodes::FileRenameFailed,
                                  "Failed to rename {} to {}: {}"_format(
                                      filename, renameTarget, ec.message()));
                }
            }
        }

        auto newFile = openFile(filename, false);
        if (newFile.isOK()) {
            invariant(file.second);
            remove_stream(file.second);
            file.second->close();
            file.second = newFile.getValue();
            add_stream(file.second);
        }
        return newFile.getStatus();
    }

    return Status::OK();
}

void FileRotateSink::consume(const boost::log::record_view& rec,
                             const string_type& formatted_string) {
    auto isFailed = [](const auto& file) { return file.second->fail(); };
    boost::log::sinks::text_ostream_backend::consume(rec, formatted_string);
    if (std::any_of(_impl->files.begin(), _impl->files.end(), isFailed)) {
        try {
            auto failedBegin =
                boost::make_filter_iterator(isFailed, _impl->files.begin(), _impl->files.end());
            auto failedEnd =
                boost::make_filter_iterator(isFailed, _impl->files.end(), _impl->files.end());

            auto getFilename = [](const auto& file) -> const auto& {
                return file.first;
            };
            auto begin = boost::make_transform_iterator(failedBegin, getFilename);
            auto end = boost::make_transform_iterator(failedEnd, getFilename);
            auto sequence = logv2::seqLog(begin, end);

            DynamicAttributes attrs;
            attrs.add("files", sequence);

            fmt::memory_buffer buffer;
            JSONFormatter(nullptr, _impl->timestampFormat)
                .format(buffer,
                        LogSeverity::Severe(),
                        LogComponent::kControl,
                        Date_t::now(),
                        4522200,
                        getThreadName(),
                        "Writing to log file failed, aborting application",
                        TypeErasedAttributeStorage(attrs),
                        LogTag::kNone,
                        nullptr /* tenantID */,
                        LogTruncation::Disabled);
            // Commented out log line below to get validation of the log id with the errorcodes
            // linter LOGV2(4522200, "Writing to log file failed, aborting application");
            std::cerr << StringData(buffer.data(), buffer.size()) << std::endl;
        } catch (const std::exception& ex) {
            std::cerr << "Caught std::exception of type " << demangleName(typeid(ex)) << ": "
                      << ex.what() << std::endl;
        } catch (const boost::exception& ex) {
            std::cerr << "Caught boost::exception of type " << demangleName(typeid(ex)) << ": "
                      << boost::diagnostic_information(ex) << std::endl;
        } catch (...) {
            std::cerr << "Caught unidentified exception" << std::endl;
        }

        printStackTrace(std::cerr);
        quickExitWithoutLogging(ExitCode::fail);
    }
}

}  // namespace mongo::logv2
