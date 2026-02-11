/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/query/query_execution_knobs_gen.h"
#include "mongo/db/query/query_integration_knobs_gen.h"
#include "mongo/db/query/query_optimization_knobs_gen.h"
#include "mongo/db/query/util/spill_util.h"
#include "mongo/db/sorter/sorter.h"
#include "mongo/db/sorter/sorter_stats.h"
#include "mongo/db/stats/counters_sort.h"
#include "mongo/db/storage/encryption_hooks.h"
#include "mongo/logv2/log.h"

#include <snappy.h>

#include <boost/filesystem/path.hpp>
#include <boost/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace MONGO_MOD_PUB mongo {
namespace sorter {

constexpr inline std::size_t kSortedFileBufferSize = size_t{64} << 10;

/**
 * Returns the current EncryptionHooks registered with the global service context.
 * Returns nullptr if the service context is not available; or if the EncyptionHooks
 * registered is not enabled.
 */
inline EncryptionHooks* getEncryptionHooksIfEnabled() {
    // Some tests may not run with a global service context.
    if (!hasGlobalServiceContext()) {
        return nullptr;
    }
    auto service = getGlobalServiceContext();
    auto encryptionHooks = EncryptionHooks::get(service);
    if (!encryptionHooks->enabled()) {
        return nullptr;
    }
    return encryptionHooks;
}

/**
 * Returns results from a sorted range within a file. Each instance is given a file name and start
 * and end offsets.
 */
template <typename Key, typename Value>
class FileIterator final : public sorter::Iterator<Key, Value> {
public:
    typedef std::pair<typename Key::SorterDeserializeSettings,
                      typename Value::SorterDeserializeSettings>
        Settings;
    typedef std::pair<Key, Value> Data;

    FileIterator(std::shared_ptr<SorterFile> file,
                 std::streamoff fileStartOffset,
                 std::streamoff fileEndOffset,
                 const Settings& settings,
                 const boost::optional<DatabaseName>& dbName,
                 const size_t checksum,
                 const SorterChecksumVersion checksumVersion)
        : _settings(settings),
          _file(std::move(file)),
          _fileStartOffset(fileStartOffset),
          _fileCurrentOffset(fileStartOffset),
          _fileEndOffset(fileEndOffset),
          _dbName(dbName),
          _checksumCalculator(checksumVersion),
          _originalChecksum(checksum) {}


    bool more() override {
        if (!_done)
            _fillBufferIfNeeded();  // may change _done
        return !_done;
    }

    Data next() override {
        // Note: calling read() on the _bufferReader buffer in the deserialize function advances the
        // buffer. Since Key comes before Value in the _bufferReader, and C++ makes no function
        // parameter evaluation order guarantees, we cannot deserialize Key and Value straight into
        // the Data constructor
        Key deserializedKey = nextWithDeferredValue();
        Value deserializedValue = getDeferredValue();
        return Data(std::move(deserializedKey), std::move(deserializedValue));
    }

    Key nextWithDeferredValue() override {
        invariant(!_done);
        _fillBufferIfNeeded();
        return Key::deserializeForSorter(*_bufferReader, _settings.first);
    }

    Value getDeferredValue() override {
        invariant(!_done);
        // Value is always in the same buffer as the Key, so no need to fill the buffer here
        return Value::deserializeForSorter(*_bufferReader, _settings.second);
    }

    const Key& peek() override {
        tasserted(ErrorCodes::NotImplemented, "peek() not implemented for FileIterator");
    }

    SorterRange getRange() const override {
        SorterRange range{
            _fileStartOffset, _fileEndOffset, static_cast<int64_t>(_originalChecksum)};
        if (_checksumCalculator.version() != SorterChecksumVersion::v1) {
            range.setChecksumVersion(_checksumCalculator.version());
        }
        return range;
    }

    bool spillable() const override {
        return false;
    }

    [[nodiscard]] std::unique_ptr<Iterator<Key, Value>> spill(
        const SortOptions& opts, const typename Sorter<Key, Value>::Settings& settings) override {
        MONGO_UNREACHABLE_TASSERT(11703803);
    }

private:
    /**
     * Attempts to refill the _bufferReader if it is empty. Expects _done to be false.
     */
    void _fillBufferIfNeeded() {
        invariant(!_done);

        if (!_bufferReader || _bufferReader->atEof()) {
            _fillBufferFromDisk();
        }
        if (_done && _originalChecksum != _checksumCalculator.checksum()) {
            fassert(31182,
                    Status(ErrorCodes::Error::ChecksumMismatch,
                           "Data read from disk does not match what was written to disk. Possible "
                           "corruption of data."));
        }
    }

    /**
     * Tries to read from disk and places any results in _bufferReader. If there is no more data to
     * read, then _done is set to true and the function returns immediately.
     */
    void _fillBufferFromDisk() {
        // The size is both written to and read from in platform-specific endian order. In the
        // unlikely event that data files are written and read by platforms of differing endianness,
        // the result will be a checksum mismatch in the worst case, which callers must recover
        // from.
        int32_t rawSize;
        _read(&rawSize, sizeof(rawSize));
        if (_done)
            return;

        // negative size means compressed
        const bool compressed = rawSize < 0;
        int32_t blockSize = std::abs(rawSize);

        _buffer = std::make_unique<char[]>(blockSize);
        _read(_buffer.get(), blockSize);
        uassert(16816, "file too short?", !_done);

        if (auto encryptionHooks = getEncryptionHooksIfEnabled()) {
            auto out = std::make_unique<char[]>(blockSize);
            DataRange outRange(out.get(), blockSize);
            Status status = encryptionHooks->unprotectTmpData(
                ConstDataRange(reinterpret_cast<const uint8_t*>(_buffer.get()), blockSize),
                &outRange,
                _dbName);
            uassert(28841,
                    str::stream() << "Failed to unprotect data: " << status.toString(),
                    status.isOK());
            blockSize = outRange.length();
            _buffer.swap(out);
        }

        if (!compressed) {
            _bufferReader = std::make_unique<BufReader>(_buffer.get(), blockSize);
            _checksumCalculator.addData(_buffer.get(), blockSize);
            return;
        }

        dassert(snappy::IsValidCompressedBuffer(_buffer.get(), blockSize));

        size_t uncompressedSize;
        uassert(17061,
                "couldn't get uncompressed length",
                snappy::GetUncompressedLength(_buffer.get(), blockSize, &uncompressedSize));

        auto decompressionBuffer = std::make_unique<char[]>(uncompressedSize);
        uassert(17062,
                "decompression failed",
                snappy::RawUncompress(_buffer.get(), blockSize, decompressionBuffer.get()));

        // hold on to decompressed data and throw out compressed data at block exit
        _buffer.swap(decompressionBuffer);
        _bufferReader = std::make_unique<BufReader>(_buffer.get(), uncompressedSize);
        _checksumCalculator.addData(_buffer.get(), uncompressedSize);
    }

    /**
     * Attempts to read data from disk. Sets _done to true when file offset reaches _fileEndOffset.
     */
    void _read(void* out, size_t size) {
        if (_fileCurrentOffset == _fileEndOffset) {
            _done = true;
            return;
        }

        invariant(_fileCurrentOffset < _fileEndOffset,
                  str::stream() << "Current file offset (" << _fileCurrentOffset
                                << ") greater than end offset (" << _fileEndOffset << ")");

        _file->read(_fileCurrentOffset, size, out);
        _fileCurrentOffset += size;
    }

    const Settings _settings;
    bool _done = false;

    std::unique_ptr<char[]> _buffer;
    std::unique_ptr<BufReader> _bufferReader;
    std::shared_ptr<SorterFile> _file;  // File containing the sorted data range.
    std::streamoff _fileStartOffset;    // File offset at which the sorted data range starts.
    std::streamoff _fileCurrentOffset;  // File offset at which we are currently reading from.
    std::streamoff _fileEndOffset;      // File offset at which the sorted data range ends.
    boost::optional<DatabaseName> _dbName;

    // Points to the beginning of a serialized key in the key-value pair currently being read, and
    // used for computing the checksum value. This is set to nullptr after reading each key-value
    // pair.
    const char* _startOfNewData = nullptr;
    // Checksum value that is updated with each read of a data object from disk. We can compare
    // this value with _originalChecksum to check for data corruption if and only if the
    // FileIterator is exhausted.
    SorterChecksumCalculator _checksumCalculator;

    // Checksum value retrieved from SortedFileWriter that was calculated as data was spilled
    // to disk. This is not modified, and is only used for comparison against _afterReadChecksum
    // when the FileIterator is exhausted to ensure no data corruption.
    const size_t _originalChecksum;
};

template <typename Key, typename Value>
class SortedFileWriter final : public mongo::SortedStorageWriter<Key, Value> {
public:
    typedef sorter::Iterator<Key, Value> Iterator;
    typedef std::pair<typename Key::SorterDeserializeSettings,
                      typename Value::SorterDeserializeSettings>
        Settings;

    explicit SortedFileWriter(const SortOptions& opts,
                              std::shared_ptr<SorterFile> file,
                              const Settings& settings = Settings());

    ~SortedFileWriter() override = default;

    void addAlreadySorted(const Key&, const Value&) override;

    std::shared_ptr<Iterator> done() override;
    std::unique_ptr<Iterator> doneUnique() override;

    void writeChunk() override;

private:
    BufBuilder _buffer;
    std::shared_ptr<SorterFile> _file;

    // Tracks where in the file we started writing the sorted data range so that the information can
    // be given to the Iterator in done().
    std::streamoff _fileStartOffset;
};


template <typename Key, typename Value>
SortedFileWriter<Key, Value>::SortedFileWriter(const SortOptions& opts,
                                               std::shared_ptr<SorterFile> file,
                                               const Settings& settings)
    : SortedStorageWriter<Key, Value>(opts, settings),
      _file(std::move(file)),
      _fileStartOffset(_file->currentOffset()) {}

template <typename Key, typename Value>
void SortedFileWriter<Key, Value>::addAlreadySorted(const Key& key, const Value& val) {
    // Add serialized key and value to the buffer.
    key.serializeForSorter(this->_buffer);
    val.serializeForSorter(this->_buffer);

    if (this->_buffer.len() > static_cast<int>(kSortedFileBufferSize))
        writeChunk();
}

template <typename Key, typename Value>
void SortedFileWriter<Key, Value>::writeChunk() {
    int32_t size = this->_buffer.len();
    char* outBuffer = this->_buffer.buf();

    if (size == 0)
        return;

    this->_checksumCalculator.addData(outBuffer, size);

    if (this->_file->getFileStats()) {
        this->_file->getFileStats()->addSpilledDataSizeUncompressed(size);
    }

    std::string compressed;
    snappy::Compress(outBuffer, size, &compressed);
    invariant(compressed.size() <= size_t(std::numeric_limits<int32_t>::max()));

    const bool shouldCompress = compressed.size() < (size_t(this->_buffer.len()) / 10 * 9);
    if (shouldCompress) {
        size = compressed.size();
        outBuffer = const_cast<char*>(compressed.data());
    }

    std::unique_ptr<char[]> out;
    if (auto encryptionHooks = getEncryptionHooksIfEnabled()) {
        size_t protectedSizeMax = size + encryptionHooks->additionalBytesForProtectedBuffer();
        out = std::make_unique<char[]>(protectedSizeMax);
        DataRange outRange(out.get(), protectedSizeMax);
        Status status = encryptionHooks->protectTmpData(
            ConstDataRange(reinterpret_cast<const uint8_t*>(outBuffer), size),
            &outRange,
            this->_opts.dbName);
        uassert(28842,
                str::stream() << "Failed to compress data: " << status.toString(),
                status.isOK());
        outBuffer = out.get();
        size = outRange.length();
    }

    // Negative size means compressed.
    int32_t signedSize = shouldCompress ? -size : size;

    // The size is both written to and read from in platform-specific endian order. In the unlikely
    // event that data files are written and read by platforms of differing endianness, the result
    // will be a read checksum mismatch in the worst case, which callers must recover from.
    _file->write(reinterpret_cast<const char*>(&signedSize), sizeof(signedSize));
    _file->write(outBuffer, size);
    sortCounters.incrementSortCountersPerSpilling(/*sortSpills=*/1, sizeof(signedSize) + size);

    this->_buffer.reset();
}

template <typename Key, typename Value>
std::shared_ptr<sorter::Iterator<Key, Value>> SortedFileWriter<Key, Value>::done() {
    writeChunk();

    return std::make_shared<sorter::FileIterator<Key, Value>>(_file,
                                                              _fileStartOffset,
                                                              _file->currentOffset(),
                                                              this->_settings,
                                                              this->_opts.dbName,
                                                              this->_checksumCalculator.checksum(),
                                                              this->_checksumCalculator.version());
}

template <typename Key, typename Value>
std::unique_ptr<sorter::Iterator<Key, Value>> SortedFileWriter<Key, Value>::doneUnique() {
    writeChunk();

    return std::make_unique<sorter::FileIterator<Key, Value>>(_file,
                                                              _fileStartOffset,
                                                              _file->currentOffset(),
                                                              this->_settings,
                                                              this->_opts.dbName,
                                                              this->_checksumCalculator.checksum(),
                                                              this->_checksumCalculator.version());
}

/**
 * A class where we declare making a writer or iterator for when the sorter is using a
 * file as its underlying storage.
 */
template <typename Key, typename Value>
class FileBasedSorterStorage : public mongo::SorterStorageBase<Key, Value> {
public:
    typedef std::pair<typename Key::SorterDeserializeSettings,
                      typename Value::SorterDeserializeSettings>
        Settings;
    using Comparator = std::function<int(const Key&, const Key&)>;

    FileBasedSorterStorage(std::shared_ptr<SorterFile> file,
                           boost::filesystem::path pathToSpillDir,
                           boost::optional<DatabaseName> dbName = boost::none,
                           SorterChecksumVersion checksumVersion = SorterChecksumVersion::v2);

    std::unique_ptr<SortedStorageWriter<Key, Value>> makeWriter(
        const SortOptions& opts, const Settings& settings = Settings()) override;

    size_t getIteratorSize() override;

    size_t getBufferSize() override;

    std::shared_ptr<Iterator<Key, Value>> getSortedIterator(const SorterRange& range,
                                                            const Settings& settings) override;

    std::string getStorageIdentifier() override;

    void keep() override;

    boost::optional<boost::filesystem::path> getSpillDirPath() override;

private:
    std::shared_ptr<SorterFile> _file;
    boost::optional<boost::filesystem::path> _pathToSpillDir;
};

template <typename Key, typename Value>
FileBasedSorterStorage<Key, Value>::FileBasedSorterStorage(std::shared_ptr<SorterFile> file,
                                                           boost::filesystem::path pathToSpillDir,
                                                           boost::optional<DatabaseName> dbName,
                                                           SorterChecksumVersion checksumVersion)
    : SorterStorageBase<Key, Value>(dbName, checksumVersion),
      _file(std::move(file)),
      _pathToSpillDir(pathToSpillDir) {}

template <typename Key, typename Value>
std::unique_ptr<SortedStorageWriter<Key, Value>> FileBasedSorterStorage<Key, Value>::makeWriter(
    const SortOptions& opts, const Settings& settings) {
    return std::make_unique<SortedFileWriter<Key, Value>>(opts, _file, settings);
}

template <typename Key, typename Value>
size_t FileBasedSorterStorage<Key, Value>::getIteratorSize() {
    return sizeof(sorter::FileIterator<Key, Value>);
}

template <typename Key, typename Value>
size_t FileBasedSorterStorage<Key, Value>::getBufferSize() {
    return kSortedFileBufferSize;
}

template <typename Key, typename Value>
std::shared_ptr<sorter::Iterator<Key, Value>> FileBasedSorterStorage<Key, Value>::getSortedIterator(
    const SorterRange& range, const Settings& settings) {
    return std::make_shared<sorter::FileIterator<Key, Value>>(
        this->_file,
        range.getStartOffset(),
        range.getEndOffset(),
        settings,
        this->getDbName(),
        range.getChecksum(),
        range.getChecksumVersion().value_or(SorterChecksumVersion::v1));
}

template <typename Key, typename Value>
void FileBasedSorterStorage<Key, Value>::keep() {
    this->_file->keep();
}

template <typename Key, typename Value>
std::string FileBasedSorterStorage<Key, Value>::getStorageIdentifier() {
    return this->_file->path().filename().string();
}

template <typename Key, typename Value>
boost::optional<boost::filesystem::path> FileBasedSorterStorage<Key, Value>::getSpillDirPath() {
    return _pathToSpillDir;
}

/**
 * How we merge spills when we use a file as the underlying storage for the sorter.
 */
template <typename Key, typename Value>
class FileBasedSorterSpiller : public mongo::SorterSpillerBase<Key, Value> {
public:
    typedef sorter::Iterator<Key, Value> Iterator;
    typedef std::pair<typename Key::SorterDeserializeSettings,
                      typename Value::SorterDeserializeSettings>
        Settings;
    using Comparator = std::function<int(const Key&, const Key&)>;

    FileBasedSorterSpiller(boost::filesystem::path tempDir,
                           SorterFileStats* fileStats,
                           boost::optional<DatabaseName> dbName = boost::none,
                           SorterChecksumVersion checksumVersion = SorterChecksumVersion::v2)
        : SorterSpillerBase<Key, Value>(std::make_unique<FileBasedSorterStorage<Key, Value>>(
              std::make_shared<SorterFile>(sorter::nextFileName(tempDir), fileStats),
              tempDir,
              dbName,
              checksumVersion)),
          _fileStats(fileStats) {}

    FileBasedSorterSpiller(std::shared_ptr<SorterFile> file,
                           boost::filesystem::path tempDir,
                           boost::optional<DatabaseName> dbName = boost::none,
                           SorterChecksumVersion checksumVersion = SorterChecksumVersion::v2)
        : SorterSpillerBase<Key, Value>(std::make_unique<FileBasedSorterStorage<Key, Value>>(
              file, tempDir, dbName, checksumVersion)),
          _fileStats(file->getFileStats()) {}

    void mergeSpills(const SortOptions& opts,
                     const Settings& settings,
                     SorterStats& stats,
                     std::vector<std::shared_ptr<sorter::Iterator<Key, Value>>>& iters,
                     Comparator comp,
                     std::size_t numTargetedSpills,
                     std::size_t numParallelSpills) override;

private:
    std::unique_ptr<SortedStorageWriter<Key, Value>> _spill(const SortOptions& opts,
                                                            const Settings& settings,
                                                            std::span<std::pair<Key, Value>> data,
                                                            uint32_t idx) override {
        std::unique_ptr<SortedStorageWriter<Key, Value>> writer =
            this->_storage->makeWriter(opts, settings);

        for (size_t i = idx; i < data.size(); ++i) {
            writer->addAlreadySorted(data[i].first, data[i].second);
        }
        return std::move(writer);
    }

    SorterFileStats* _fileStats;
};

template <typename Key, typename Value>
void FileBasedSorterSpiller<Key, Value>::mergeSpills(
    const SortOptions& opts,
    const Settings& settings,
    SorterStats& sorterStats,
    std::vector<std::shared_ptr<sorter::Iterator<Key, Value>>>& iters,
    Comparator comp,
    std::size_t numTargetedSpills,
    std::size_t numParallelSpills) {
    using File = SorterFile;

    std::shared_ptr<File> newSpillsFile =
        std::make_shared<File>(sorter::nextFileName(*opts.tempDir), _fileStats);
    FileBasedSorterStorage<Key, Value> sorterStorage(newSpillsFile, *opts.tempDir);

    std::vector<std::shared_ptr<Iterator>> iterators;
    while (iters.size() > numTargetedSpills) {
        iterators.swap(iters);

        newSpillsFile = std::make_shared<File>(sorter::nextFileName(*opts.tempDir), _fileStats);
        LOGV2_DEBUG(6033103,
                    1,
                    "Created new intermediate file for merged spills",
                    "path"_attr = newSpillsFile->path().string());

        for (std::size_t i = 0; i < iterators.size(); i += numParallelSpills) {
            auto count = std::min(numParallelSpills, iterators.size() - i);
            auto spillsToMerge = std::span(iterators).subspan(i, count);

            // Since we are merging the spills to a new file, we make sure we have sufficient
            // available disk space
            int64_t minRequiredDiskSpace = 0;
            for (auto&& it : spillsToMerge) {
                minRequiredDiskSpace +=
                    it->getRange().getEndOffset() - it->getRange().getStartOffset();
            }
            minRequiredDiskSpace = std::max(
                minRequiredDiskSpace,
                static_cast<int64_t>(internalQuerySpillingMinAvailableDiskSpaceBytes.load()));
            uassertStatusOK(
                ensureSufficientDiskSpaceForSpilling(*opts.tempDir, minRequiredDiskSpace));

            LOGV2_DEBUG(
                6033102, 2, "Merging spills", "beginIdx"_attr = i, "endIdx"_attr = i + count - 1);

            auto mergeIterator = sorter::merge<Key, Value>(spillsToMerge, opts, comp);
            sorterStorage = FileBasedSorterStorage<Key, Value>(newSpillsFile, *opts.tempDir);
            std::unique_ptr<SortedStorageWriter<Key, Value>> writer =
                sorterStorage.makeWriter(opts, settings);
            uint64_t pairCount = 0;
            while (mergeIterator->more()) {
                auto pair = mergeIterator->next();
                writer->addAlreadySorted(pair.first, pair.second);
                ++pairCount;
            }
            iters.push_back(writer->done());
            sorterStats.incrementSpilledRanges();
            sorterStats.incrementSpilledKeyValuePairs(pairCount);
        }
        iterators.clear();

        LOGV2_DEBUG(6033101,
                    1,
                    "Merged spills",
                    "currentNumSpills"_attr = iters.size(),
                    "targetNumSpills"_attr = numTargetedSpills);
    }

    LOGV2_INFO(6033100, "Finished merging spills");
    this->_storage = std::make_unique<FileBasedSorterStorage<Key, Value>>(std::move(sorterStorage));
}

}  // namespace sorter
}  // namespace MONGO_MOD_PUB mongo
#undef MONGO_LOGV2_DEFAULT_COMPONENT
