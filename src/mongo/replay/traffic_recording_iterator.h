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

#pragma once

#include "mongo/replay/recording_reader.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <filesystem>
#include <iterator>

namespace mongo {


/**
 * Iterator used to read TrafficReaderPackets incrementally from
 * a single recording file.
 *
 * This internally mmemory maps the file.
 *
 * The TrafficReaderPacket holds views into the memory mapped data.
 * It is guaranteed to be valid until the iterator is advanced, or while
 * the appropriate file is kept mapped by other means.
 */
class RecordingIterator {
public:
    using iterator_category = std::input_iterator_tag;
    using value_type = TrafficReaderPacket;
    using difference_type = std::ptrdiff_t;
    using pointer = const value_type*;
    using reference = const value_type&;

    RecordingIterator() = default;
    RecordingIterator(std::filesystem::path file);
    RecordingIterator(boost::iostreams::mapped_file_source mappedFile);

    RecordingIterator& operator++();

    bool operator==(std::default_sentinel_t) const;

    bool operator!=(std::default_sentinel_t) const;

    reference operator*() const;

    pointer operator->() const;

private:
    RecordingReader _reader;
    boost::optional<value_type> _currentValue;
};

inline auto begin(RecordingIterator iter) {
    return iter;
}
inline auto end(RecordingIterator) {
    return std::default_sentinel;
}


/**
 * Abstract representation of an ordered collection of files.
 *
 * Provides memory mapped access to each file via `get()`.
 * Promises not to map a single file multiple times at the same time.
 *
 * Abstract to allow future implementations which e.g., fetch files
 * from S3 on-demand.
 */
class FileSet {
public:
    virtual ~FileSet() = default;
    /**
     * Acquire (read only) memory mapped access to the file at `index`.
     *
     * If the file is already mapped, shared access will be provided to the same map;
     * it will not be blindly re-mapped.
     */
    virtual std::shared_ptr<boost::iostreams::mapped_file_source> get(size_t index) = 0;

    /**
     * Check if this FileSet contains any files.
     */
    virtual bool empty() const = 0;

    /**
     * Construct a FileSet representing the `.bin` recording files contained within the provided
     * directory.
     */
    static std::shared_ptr<FileSet> from_directory(const std::filesystem::path& dir);
};

/**
 * Iterator used to read TrafficReaderPackets incrementally from
 * a _collection_ of recording files, sourced from a single node.
 *
 * Uses RecordingIterator internally; see that class for validity and
 * restrictions.
 */
class RecordingSetIterator {
public:
    using iterator_category = std::input_iterator_tag;
    using value_type = TrafficReaderPacket;
    using difference_type = std::ptrdiff_t;
    using pointer = const value_type*;
    using reference = const value_type&;

    RecordingSetIterator() = default;
    RecordingSetIterator(std::shared_ptr<FileSet> fileset);
    RecordingSetIterator(std::filesystem::path dir);

    RecordingSetIterator& operator++();

    bool operator==(std::default_sentinel_t) const;

    bool operator!=(std::default_sentinel_t) const;

    reference operator*() const;

    pointer operator->() const;

private:
    std::shared_ptr<FileSet> _files;
    std::shared_ptr<boost::iostreams::mapped_file_source> _activeFile;
    RecordingIterator _recordingIter;
    size_t _fileIndex = 0;
};

inline auto begin(RecordingSetIterator iter) {
    return iter;
}
inline auto end(RecordingSetIterator) {
    return std::default_sentinel;
}

}  // namespace mongo
