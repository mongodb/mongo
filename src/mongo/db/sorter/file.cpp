/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/db/sorter/file.h"

#include <boost/filesystem/operations.hpp>

#include "mongo/util/destructor_guard.h"

namespace mongo::sorter {
namespace {
// We need to use the "real" errno everywhere, not GetLastError() on Windows.
std::string getError() {
    int errnoCopy = errno;
    StringBuilder sb;
    sb << "errno:" << errnoCopy << ' ' << strerror(errnoCopy);
    return sb.str();
}
}  // namespace

File::~File() {
    if (_keep) {
        return;
    }

    if (_file.is_open()) {
        DESTRUCTOR_GUARD(_file.exceptions(std::ios::failbit));
        DESTRUCTOR_GUARD(_file.close());
    }

    DESTRUCTOR_GUARD(boost::filesystem::remove(_path));
}

void File::read(std::streamoff offset, std::streamsize size, void* out) {
    if (!_file.is_open()) {
        _open();
    }

    if (_offset != -1) {
        _file.exceptions(std::ios::goodbit);
        _file.flush();
        _offset = -1;

        uassert(5479100,
                str::stream() << "Error flushing file " << _path.string() << ": " << getError(),
                _file);
    }

    _file.seekg(offset);
    _file.read(reinterpret_cast<char*>(out), size);

    uassert(16817,
            str::stream() << "Error reading file " << _path.string() << ": " << getError(),
            _file);

    invariant(_file.gcount() == size,
              str::stream() << "Number of bytes read (" << _file.gcount()
                            << ") not equal to expected number (" << size << ")");

    uassert(51049,
            str::stream() << "Error reading file " << _path.string() << ": " << getError(),
            _file.tellg() >= 0);
}

void File::write(const char* data, std::streamsize size) {
    _ensureOpenForWriting();

    try {
        _file.write(data, size);
        _offset += size;
    } catch (const std::system_error& ex) {
        if (ex.code() == std::errc::no_space_on_device) {
            uasserted(ErrorCodes::OutOfDiskSpace,
                      str::stream() << ex.what() << ": " << _path.string());
        }
        uasserted(5642403,
                  str::stream() << "Error writing to file " << _path.string() << ": "
                                << getError());
    } catch (const std::exception&) {
        uasserted(16821,
                  str::stream() << "Error writing to file " << _path.string() << ": "
                                << getError());
    }
}

std::streamoff File::currentOffset() {
    _ensureOpenForWriting();
    return _offset;
}

void File::_open() {
    invariant(!_file.is_open());

    boost::filesystem::create_directories(_path.parent_path());

    // We open the provided file in append mode so that SortedFileWriter instances can share
    // the same file, used serially. We want to share files in order to stay below system
    // open file limits.
    _file.open(_path.string(), std::ios::app | std::ios::binary | std::ios::in | std::ios::out);

    uassert(16818,
            str::stream() << "Error opening file " << _path.string() << ": " << getError(),
            _file.good());
}

void File::_ensureOpenForWriting() {
    invariant(_offset != -1 || !_file.is_open());

    if (_file.is_open()) {
        return;
    }

    _open();
    _file.exceptions(std::ios::failbit | std::ios::badbit);
    _offset = boost::filesystem::file_size(_path);
}
}  // namespace mongo::sorter
