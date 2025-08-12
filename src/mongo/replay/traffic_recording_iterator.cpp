/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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
#include "mongo/replay/traffic_recording_iterator.h"

#include <cstddef>
#include <filesystem>
#include <iterator>
#include <memory>
#include <mutex>

namespace mongo {

RecordingIterator::RecordingIterator(std::filesystem::path file)
    // Explicit boost::iostreams::detail::path required for MSVC and current version of boost to
    // compile.
    : RecordingIterator(
          boost::iostreams::mapped_file_source(boost::iostreams::detail::path(file.string()))) {}

RecordingIterator::RecordingIterator(boost::iostreams::mapped_file_source mappedFile)
    : _reader(mappedFile) {
    uassert(ErrorCodes::InvalidPath, "Unable to map file", mappedFile.is_open());
    // Read the first value, if any.
    ++*this;
}

RecordingIterator& RecordingIterator::operator++() {
    _currentValue = _reader.readPacket();
    return *this;
}

bool RecordingIterator::operator==(std::default_sentinel_t) const {
    return !_currentValue.has_value();
}

bool RecordingIterator::operator!=(std::default_sentinel_t) const {
    return !(*this == std::default_sentinel);
}

RecordingIterator::reference RecordingIterator::operator*() const {
    return *_currentValue;
}

RecordingIterator::pointer RecordingIterator::operator->() const {
    return &*_currentValue;
}

class LocalFileSet : public FileSet {
public:
    LocalFileSet(const std::filesystem::path& dir) {
        uassert(ErrorCodes::InvalidPath,
                str::stream() << "Replay expected directory: " << dir.string(),
                std::filesystem::is_directory(dir));
        for (const auto& entry : std::filesystem::directory_iterator(dir)) {
            if (entry.path().extension() == ".bin") {
                auto pathStr = entry.path().string();
                // Store the filenames, but don't map them yet.
                // There could be many, but only a handful need to be mapped at a time.
                files.push_back({pathStr, {}});
            }
        }
        std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) {
            return std::get<0>(a) < std::get<0>(b);
        });
    }

    std::shared_ptr<boost::iostreams::mapped_file_source> get(size_t index) override {
        if (index >= files.size()) {
            return {};
        }
        std::unique_lock ul(mutex);
        auto& [filename, ptr] = files[index];
        if (!ptr) {
            ptr = std::make_shared<boost::iostreams::mapped_file_source>(filename);
            uassert(ErrorCodes::FileNotOpen, "Failed to map file", ptr->is_open());
        }
        return ptr;
    }

    bool empty() const override {
        return files.empty();
    }

    std::mutex mutex;
    // Currently files remain mapped for the lifetime of the LocalFileSet.
    // This is required as replayThread dispatches ReplayCommand objects to other threads;
    // this is not an owning type, and does not extend the life of the mmapped data.
    // TODO SERVER-106702: Change this to weak ownership here, and have worker threads maintain an
    // iterator,
    //       so files remain mapped while in use, but can be unmmapped when no longer referenced by
    //       any thread.
    std::vector<std::pair<std::string, std::shared_ptr<boost::iostreams::mapped_file_source>>>
        files;
};

std::shared_ptr<FileSet> FileSet::from_directory(const std::filesystem::path& dir) {
    return std::make_shared<LocalFileSet>(dir);
}


RecordingSetIterator::RecordingSetIterator(std::shared_ptr<FileSet> fileset)
    : _files(std::move(fileset)) {
    _activeFile = _files->get(0);
    if (_activeFile) {
        _recordingIter = *_activeFile;
    }
}

RecordingSetIterator::RecordingSetIterator(std::filesystem::path dir)
    : RecordingSetIterator(FileSet::from_directory(dir)) {}

RecordingSetIterator& RecordingSetIterator::operator++() {
    ++_recordingIter;
    while (_recordingIter == std::default_sentinel) {
        _activeFile = _files->get(++_fileIndex);
        if (!_activeFile) {
            break;
        }
        _recordingIter = *_activeFile;
    }
    return *this;
}

bool RecordingSetIterator::operator==(std::default_sentinel_t) const {
    return !_activeFile;
}

bool RecordingSetIterator::operator!=(std::default_sentinel_t) const {
    return !(*this == std::default_sentinel);
}

RecordingSetIterator::reference RecordingSetIterator::operator*() const {
    return *_recordingIter;
}

RecordingSetIterator::pointer RecordingSetIterator::operator->() const {
    return &*_recordingIter;
}


}  // namespace mongo
