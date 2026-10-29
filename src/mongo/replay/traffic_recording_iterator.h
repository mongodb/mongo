// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
