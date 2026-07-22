// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/sorter/file.h"

#include "mongo/base/error_codes.h"
#include "mongo/db/stats/counters_sort.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/errno_util.h"
#include "mongo/util/file.h"
#include "mongo/util/str.h"

#include <boost/filesystem/operations.hpp>

namespace mongo::sorter {

File::File(boost::filesystem::path path, SorterFileStats* stats) : _stats(stats), _path(path) {
    invariant(!_path.empty());
    if (boost::filesystem::exists(_path) && boost::filesystem::is_regular_file(_path)) {
        // This File is adopting an already-populated on-disk file (e.g. resuming persisted spill
        // state). Account for its existing size in both the cumulative stats and the live gauge.
        auto existingSize = static_cast<long long>(boost::filesystem::file_size(_path));
        if (_stats) {
            _stats->addSpilledDataSize(existingSize);
        }
        _addToStorageSizeGauge(existingSize);
    }
}

File::~File() {
    if (_stats && _file.is_open()) {
        _stats->closed.addAndFetch(1);
    }

    if (_keep) {
        if (!_file.is_open()) {
            return;
        }
        try {
            _file.flush();
        } catch (...) {
            reportFailedDestructor(MONGO_SOURCE_LOCATION());
        }

        mongo::File fileForFsync;
        fileForFsync.open(_path.string().c_str());
        if (fileForFsync.is_open()) {
            fileForFsync.fsync();
        }

        return;
    }

    if (_file.is_open()) {
        try {
            _file.exceptions(std::ios::failbit);
        } catch (...) {
            reportFailedDestructor(MONGO_SOURCE_LOCATION());
        }
        try {
            _file.close();
        } catch (...) {
            reportFailedDestructor(MONGO_SOURCE_LOCATION());
        }
    }

    try {
        boost::filesystem::remove(_path);
        fileSpillingMetrics.fileSpilledStorageSize.decrement(_spilledBytes);
    } catch (...) {
        reportFailedDestructor(MONGO_SOURCE_LOCATION());
    }
}

void File::read(std::streamoff offset, std::streamsize size, void* out) {
    if (!_file.is_open()) {
        _open();
    }

    // If the _offset is not -1, we may have written data to it, so we must flush.
    if (_offset != -1) {
        _file.exceptions(std::ios::goodbit);
        _file.flush();
        _offset = -1;

        uassert(5479100,
                str::stream() << "Error flushing file " << _path.string() << ": "
                              << errorMessage(_getErrorCode()),
                _file);
    }

    _file.seekg(offset);
    _file.read(reinterpret_cast<char*>(out), size);

    uassert(16817,
            str::stream() << "Error reading file " << _path.string() << ": "
                          << errorMessage(_getErrorCode()),
            _file);

    invariant(_file.gcount() == size,
              str::stream() << "Number of bytes read (" << _file.gcount()
                            << ") not equal to expected number (" << size << ")");

    uassert(51049,
            str::stream() << "Error reading file " << _path.string() << ": "
                          << errorMessage(_getErrorCode()),
            _file.tellg() >= 0);
}

void File::write(const char* data, std::streamsize size) {
    _ensureOpenForWriting();

    try {
        _file.write(data, size);
        _offset += size;
        if (_stats) {
            this->_stats->addSpilledDataSize(size);
        };
        _addToStorageSizeGauge(size);
    } catch (const std::system_error& ex) {
        if (ex.code() == std::errc::no_space_on_device) {
            uasserted(ErrorCodes::OutOfDiskSpace,
                      str::stream() << ex.what() << ": " << _path.string());
        }
        uasserted(5642403,
                  str::stream() << "Error writing to file " << _path.string() << ": " << ex.what());
    } catch (const std::exception& ex) {
        uasserted(16821,
                  str::stream() << "Error writing to file " << _path.string() << ": " << ex.what());
    }
}

std::streamoff File::currentOffset() {
    _ensureOpenForWriting();
    invariant(_offset >= 0);
    return _offset;
}

SorterFileStats* File::getFileStats() {
    return _stats;
}

void File::_open() {
    invariant(!_file.is_open());

    boost::filesystem::create_directories(_path.parent_path());

    // We open the provided file in append mode so that SortedFileWriter instances can share
    // the same file, used serially. We want to share files in order to stay below system
    // open file limits.
    _file.open(_path.string(), std::ios::app | std::ios::binary | std::ios::in | std::ios::out);

    uassert(16818,
            str::stream() << "Error opening file " << _path.string() << ": "
                          << errorMessage(_getErrorCode()),
            _file.good());

    if (_stats) {
        _stats->opened.addAndFetch(1);
    }
}

void File::_ensureOpenForWriting() {
    if (!_file.is_open()) {
        _open();
    }

    // If we are opening the file for the first time, or if we previously flushed and switched to
    // read mode, we need to set the _offset to the file size.
    if (_offset == -1) {
        _file.exceptions(std::ios::failbit | std::ios::badbit);
        _offset = boost::filesystem::file_size(_path);
        _file.seekp(_offset);
    }
}

void File::_addToStorageSizeGauge(long long size) {
    _spilledBytes += size;
    fileSpillingMetrics.fileSpilledStorageSize.increment(size);
}

std::error_code File::_getErrorCode() {
    auto err = lastPosixError();
    // If no posix error, check for iostream error.
    if (!err && (_file.fail() || _file.bad())) {
        return std::make_error_code(std::io_errc::stream);
    }
    return err;
}

}  // namespace mongo::sorter
