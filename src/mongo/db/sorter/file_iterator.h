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

#pragma once

#include "mongo/db/sorter/sorted_data_iterator.h"

#include "mongo/db/sorter/compression.h"
#include "mongo/db/sorter/file.h"
#include "mongo/db/sorter/util.h"

namespace mongo::sorter {
/**
 * Iterates over a sorted range within a file.
 */
template <typename Key, typename Value>
class FileIterator : public SortedDataIterator<Key, Value> {
public:
    using Base = SortedDataIterator<Key, Value>;
    using Data = typename Base::Data;
    using Settings = typename Base::Settings;

    FileIterator(File* file,
                 std::streamoff fileStartOffset,
                 std::streamoff fileEndOffset,
                 const uint32_t checksum,
                 const Settings& settings,
                 const boost::optional<std::string>& dbName)
        : _settings(settings),
          _file(file),
          _fileStartOffset(fileStartOffset),
          _fileCurrentOffset(fileStartOffset),
          _fileEndOffset(fileEndOffset),
          _originalChecksum(checksum),
          _dbName(dbName) {}

    ~FileIterator() {
        // If the file iterator reads through all data objects, we can ensure non-corrupt data by
        // comparing the newly calculated checksum with the original checksum from the data written
        // to disk. Some iterators do not read back all data from the file, which prohibits the
        // _afterReadChecksum from obtaining all the information needed. Thus, we only fassert if
        // all data that was written to disk is read back and the checksums are not equivalent.
        if (!more() && _bufferReader->atEof() && (_originalChecksum != _afterReadChecksum)) {
            fassert(31182,
                    Status(ErrorCodes::Error::ChecksumMismatch,
                           "Data read from disk does not match what was written to disk. Possible "
                           "corruption of data."));
        }
    }

    bool more() const {
        return !_bufferReader || !_bufferReader->atEof() || _fileCurrentOffset < _fileEndOffset;
    }

    Data next() {
        if (!_bufferReader || _bufferReader->atEof()) {
            _fillBuffer();
        }

        const char* startOfNewData = static_cast<const char*>(_bufferReader->pos());

        // Note: calling read() on the _bufferReader buffer in the deserialize function advances the
        // buffer. Since Key comes before Value in the _bufferReader, and C++ makes no function
        // parameter evaluation order guarantees, we cannot deserialize Key and Value straight into
        // the Data constructor
        auto first = Key::deserializeForSorter(*_bufferReader, _settings.first);
        auto second = Value::deserializeForSorter(*_bufferReader, _settings.second);

        // The difference of _bufferReader's position before and after reading the data
        // will provide the length of the data that was just read.
        const char* endOfNewData = static_cast<const char*>(_bufferReader->pos());

        _afterReadChecksum =
            addDataToChecksum(startOfNewData, endOfNewData - startOfNewData, _afterReadChecksum);

        return {std::move(first), std::move(second)};
    }

    SorterRange getRange() const override {
        return {_fileStartOffset, _fileEndOffset, _originalChecksum};
    }

private:
    /**
     * Fills the buffer by reading from disk.
     */
    void _fillBuffer() {
        int32_t rawSize;
        _read(&rawSize, sizeof(rawSize));

        // Negative size means compressed.
        const bool compressed = rawSize < 0;
        int32_t blockSize = std::abs(rawSize);

        _buffer.reset(new char[blockSize]);
        _read(_buffer.get(), blockSize);

        if (auto encryptionHooks = getEncryptionHooksIfEnabled()) {
            std::unique_ptr<char[]> out(new char[blockSize]);
            size_t outLen;
            Status status =
                encryptionHooks->unprotectTmpData(reinterpret_cast<const uint8_t*>(_buffer.get()),
                                                  blockSize,
                                                  reinterpret_cast<uint8_t*>(out.get()),
                                                  blockSize,
                                                  &outLen,
                                                  _dbName);
            uassert(28841,
                    str::stream() << "Failed to unprotect data: " << status.toString(),
                    status.isOK());
            blockSize = outLen;
            _buffer.swap(out);
        }

        if (!compressed) {
            _bufferReader.reset(new BufReader(_buffer.get(), blockSize));
            return;
        }

        dassert(isValidCompressedBuffer(_buffer.get(), blockSize));

        size_t uncompressedSize;
        uassert(17061,
                "Failed to get uncompressed size",
                getUncompressedSize(_buffer.get(), blockSize, &uncompressedSize));

        std::unique_ptr<char[]> decompressionBuffer(new char[uncompressedSize]);
        uassert(17062,
                "Failed to decompress",
                decompress(_buffer.get(), blockSize, decompressionBuffer.get()));

        // Hold on to decompressed data and throw out compressed data at block exit.
        _buffer.swap(decompressionBuffer);
        _bufferReader.reset(new BufReader(_buffer.get(), uncompressedSize));
    }

    /**
     * Reads data from disk.
     */
    void _read(void* out, size_t size) {
        invariant(_fileCurrentOffset < _fileEndOffset,
                  str::stream() << "Current file offset (" << _fileCurrentOffset
                                << ") greater than end offset (" << _fileEndOffset << ")");

        _file->read(_fileCurrentOffset, size, out);
        _fileCurrentOffset += size;
    }

    const Settings _settings;

    std::unique_ptr<char[]> _buffer;
    std::unique_ptr<BufReader> _bufferReader;

    File* _file;                        // File containing the sorted data range.
    std::streamoff _fileStartOffset;    // File offset at which the sorted data range starts.
    std::streamoff _fileCurrentOffset;  // File offset at which we are currently reading from.
    std::streamoff _fileEndOffset;      // File offset at which the sorted data range ends.

    // Checksum value retrieved from SortedFileWriter that was calculated as data was spilled
    // to disk. This is not modified, and is only used for comparison against _afterReadChecksum
    // when the FileIterator is exhausted to ensure no data corruption.
    const uint32_t _originalChecksum;

    // Checksum value that is updated with each read of a data object from disk. We can compare
    // this value with _originalChecksum to check for data corruption if and only if the
    // FileIterator is exhausted.
    uint32_t _afterReadChecksum = 0;

    boost::optional<std::string> _dbName;
};
}  // namespace mongo::sorter
