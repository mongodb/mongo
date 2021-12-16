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

#include "mongo/db/sorter/file.h"
#include "mongo/db/sorter/file_iterator.h"
#include "mongo/db/sorter/options.h"
#include "mongo/db/sorter/sorted_data_iterator.h"
#include "mongo/db/sorter/util.h"
#include "mongo/s/is_mongos.h"

namespace mongo::sorter {
template <typename Key, typename Value>
class SortedFileWriter {
public:
    using Iterator = SortedDataIterator<Key, Value>;
    using Settings = std::pair<typename Key::SorterDeserializeSettings,
                               typename Value::SorterDeserializeSettings>;

    SortedFileWriter(File* file,
                     const boost::optional<std::string>& dbName = boost::none,
                     const Settings& settings = Settings())
        : _settings(settings),
          _file(file),
          _fileStartOffset(_file->currentOffset()),
          _dbName(dbName) {
        invariant(!isMongos());
    }

    SortedFileWriter(const SortedFileWriter&) = delete;
    SortedFileWriter& operator=(const SortedFileWriter&) = delete;

    void addAlreadySorted(const Key& key, const Value& val) {
        // Offset that points to the place in the buffer where a new data object will be stored.
        int nextObjPos = _buffer.len();

        // Add serialized key and value to the buffer.
        key.serializeForSorter(_buffer);
        val.serializeForSorter(_buffer);

        // Serializing the key and value grows the buffer, but _buffer.buf() still points to the
        // beginning. Use _buffer.len() to determine portion of buffer containing new datum.
        _checksum =
            addDataToChecksum(_buffer.buf() + nextObjPos, _buffer.len() - nextObjPos, _checksum);

        if (_buffer.len() > 64 * 1024) {
            _spill();
        }
    }

    std::unique_ptr<Iterator> done() {
        _spill();

        return std::make_unique<FileIterator<Key, Value>>(
            _file, _fileStartOffset, _file->currentOffset(), _checksum, _settings, _dbName);
    }

private:
    void _spill() {
        int32_t size = _buffer.len();
        char* outBuffer = _buffer.buf();

        if (size == 0) {
            return;
        }

        std::string compressed;
        compress(outBuffer, size, &compressed);
        invariant(compressed.size() <= size_t(std::numeric_limits<int32_t>::max()));

        const bool shouldCompress = compressed.size() < size_t(_buffer.len() / 10 * 9);
        if (shouldCompress) {
            size = compressed.size();
            outBuffer = const_cast<char*>(compressed.data());
        }

        std::unique_ptr<char[]> out;
        if (auto encryptionHooks = getEncryptionHooksIfEnabled()) {
            size_t protectedSizeMax = size + encryptionHooks->additionalBytesForProtectedBuffer();
            out.reset(new char[protectedSizeMax]);
            size_t resultLen;
            Status status =
                encryptionHooks->protectTmpData(reinterpret_cast<const uint8_t*>(outBuffer),
                                                size,
                                                reinterpret_cast<uint8_t*>(out.get()),
                                                protectedSizeMax,
                                                &resultLen,
                                                _dbName);
            uassert(28842,
                    str::stream() << "Failed to compress data: " << status.toString(),
                    status.isOK());
            outBuffer = out.get();
            size = resultLen;
        }

        // Negative size means compressed.
        size = shouldCompress ? -size : size;
        _file->write(reinterpret_cast<const char*>(&size), sizeof(size));
        _file->write(outBuffer, std::abs(size));

        _buffer.reset();
    }

    const Settings _settings;
    File* _file;
    BufBuilder _buffer;

    // Keeps track of the hash of all data objects spilled to disk. Passed to the FileIterator
    // to ensure data has not been corrupted after reading from disk.
    uint32_t _checksum = 0;

    // Tracks where in the file we started writing the sorted data range so that the information can
    // be given to the Iterator in done().
    std::streamoff _fileStartOffset;

    boost::optional<std::string> _dbName;
};
}  // namespace mongo::sorter
