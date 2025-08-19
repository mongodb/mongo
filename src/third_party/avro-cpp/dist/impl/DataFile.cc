/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "DataFile.hh"
#include "Compiler.hh"
#include "Exception.hh"

#include <sstream>

#include <boost/crc.hpp> // for boost::crc_32_type
#include <boost/iostreams/device/file.hpp>
#include <boost/iostreams/filter/gzip.hpp>
#include <boost/iostreams/filter/zlib.hpp>
#include <boost/random/mersenne_twister.hpp>

#ifdef SNAPPY_CODEC_AVAILABLE
#include <snappy.h>
#endif

namespace avro {
using std::copy;
using std::istringstream;
using std::ostringstream;
using std::string;
using std::unique_ptr;
using std::vector;

using std::array;

namespace {
const string AVRO_SCHEMA_KEY("avro.schema");
const string AVRO_CODEC_KEY("avro.codec");
const string AVRO_NULL_CODEC("null");
const string AVRO_DEFLATE_CODEC("deflate");

#ifdef SNAPPY_CODEC_AVAILABLE
const string AVRO_SNAPPY_CODEC = "snappy";
#endif

const size_t minSyncInterval = 32;
const size_t maxSyncInterval = 1u << 30;

boost::iostreams::zlib_params get_zlib_params() {
    boost::iostreams::zlib_params ret;
    ret.method = boost::iostreams::zlib::deflated;
    ret.noheader = true;
    return ret;
}
} // namespace

DataFileWriterBase::DataFileWriterBase(const char *filename, const ValidSchema &schema, size_t syncInterval,
                                       Codec codec) : filename_(filename),
                                                      schema_(schema),
                                                      encoderPtr_(binaryEncoder()),
                                                      syncInterval_(syncInterval),
                                                      codec_(codec),
                                                      stream_(fileOutputStream(filename)),
                                                      buffer_(memoryOutputStream()),
                                                      sync_(makeSync()),
                                                      objectCount_(0),
                                                      lastSync_(0) {
    init(schema, syncInterval, codec);
}

DataFileWriterBase::DataFileWriterBase(std::unique_ptr<OutputStream> outputStream,
                                       const ValidSchema &schema, size_t syncInterval, Codec codec) : filename_(),
                                                                                                      schema_(schema),
                                                                                                      encoderPtr_(binaryEncoder()),
                                                                                                      syncInterval_(syncInterval),
                                                                                                      codec_(codec),
                                                                                                      stream_(std::move(outputStream)),
                                                                                                      buffer_(memoryOutputStream()),
                                                                                                      sync_(makeSync()),
                                                                                                      objectCount_(0),
                                                                                                      lastSync_(0) {
    init(schema, syncInterval, codec);
}

void DataFileWriterBase::init(const ValidSchema &schema, size_t syncInterval, const Codec &codec) {
    if (syncInterval < minSyncInterval || syncInterval > maxSyncInterval) {
        throw Exception(
            "Invalid sync interval: {}. Should be between {} and {}",
            syncInterval, minSyncInterval, maxSyncInterval);
    }
    setMetadata(AVRO_CODEC_KEY, AVRO_NULL_CODEC);

    if (codec_ == NULL_CODEC) {
        setMetadata(AVRO_CODEC_KEY, AVRO_NULL_CODEC);
    } else if (codec_ == DEFLATE_CODEC) {
        setMetadata(AVRO_CODEC_KEY, AVRO_DEFLATE_CODEC);
#ifdef SNAPPY_CODEC_AVAILABLE
    } else if (codec_ == SNAPPY_CODEC) {
        setMetadata(AVRO_CODEC_KEY, AVRO_SNAPPY_CODEC);
#endif
    } else {
        throw Exception("Unknown codec: {}", int(codec));
    }
    setMetadata(AVRO_SCHEMA_KEY, schema.toJson(false));

    writeHeader();
    encoderPtr_->init(*buffer_);

    lastSync_ = stream_->byteCount();
}

DataFileWriterBase::~DataFileWriterBase() {
    if (stream_) {
        try {
            close();
        } catch (...) {}
    }
}

void DataFileWriterBase::close() {
    flush();
    stream_.reset();
}

void DataFileWriterBase::sync() {
    encoderPtr_->flush();

    encoderPtr_->init(*stream_);
    avro::encode(*encoderPtr_, objectCount_);
    if (codec_ == NULL_CODEC) {
        int64_t byteCount = buffer_->byteCount();
        avro::encode(*encoderPtr_, byteCount);
        encoderPtr_->flush();
        std::unique_ptr<InputStream> in = memoryInputStream(*buffer_);
        copy(*in, *stream_);
    } else if (codec_ == DEFLATE_CODEC) {
        std::vector<char> buf;
        {
            boost::iostreams::filtering_ostream os;
            os.push(boost::iostreams::zlib_compressor(get_zlib_params()));
            os.push(boost::iostreams::back_inserter(buf));
            const uint8_t *data;
            size_t len;

            std::unique_ptr<InputStream> input = memoryInputStream(*buffer_);
            while (input->next(&data, &len)) {
                boost::iostreams::write(os, reinterpret_cast<const char *>(data), len);
            }
        } // make sure all is flushed
        std::unique_ptr<InputStream> in = memoryInputStream(
            reinterpret_cast<const uint8_t *>(buf.data()), buf.size());
        int64_t byteCount = buf.size();
        avro::encode(*encoderPtr_, byteCount);
        encoderPtr_->flush();
        copy(*in, *stream_);
#ifdef SNAPPY_CODEC_AVAILABLE
    } else if (codec_ == SNAPPY_CODEC) {
        std::vector<char> temp;
        std::string compressed;
        boost::crc_32_type crc;
        {
            boost::iostreams::filtering_ostream os;
            os.push(boost::iostreams::back_inserter(temp));
            const uint8_t *data;
            size_t len;

            std::unique_ptr<InputStream> input = memoryInputStream(*buffer_);
            while (input->next(&data, &len)) {
                boost::iostreams::write(os, reinterpret_cast<const char *>(data),
                                        len);
            }
        } // make sure all is flushed

        crc.process_bytes(reinterpret_cast<const char *>(temp.data()),
                          temp.size());
        // For Snappy, add the CRC32 checksum
        int32_t checksum = crc();

        // Now compress
        size_t compressed_size = snappy::Compress(
            reinterpret_cast<const char *>(temp.data()), temp.size(),
            &compressed);
        temp.clear();
        {
            boost::iostreams::filtering_ostream os;
            os.push(boost::iostreams::back_inserter(temp));
            boost::iostreams::write(os, compressed.c_str(), compressed_size);
        }
        temp.push_back(static_cast<char>((checksum >> 24) & 0xFF));
        temp.push_back(static_cast<char>((checksum >> 16) & 0xFF));
        temp.push_back(static_cast<char>((checksum >> 8) & 0xFF));
        temp.push_back(static_cast<char>(checksum & 0xFF));
        std::unique_ptr<InputStream> in = memoryInputStream(
            reinterpret_cast<const uint8_t *>(temp.data()), temp.size());
        int64_t byteCount = temp.size();
        avro::encode(*encoderPtr_, byteCount);
        encoderPtr_->flush();
        copy(*in, *stream_);
#endif
    }

    encoderPtr_->init(*stream_);
    avro::encode(*encoderPtr_, sync_);
    encoderPtr_->flush();

    lastSync_ = stream_->byteCount();

    buffer_ = memoryOutputStream();
    encoderPtr_->init(*buffer_);
    objectCount_ = 0;
}

void DataFileWriterBase::syncIfNeeded() {
    encoderPtr_->flush();
    if (buffer_->byteCount() >= syncInterval_) {
        sync();
    }
}

uint64_t DataFileWriterBase::getCurrentBlockStart() const {
    return lastSync_;
}

void DataFileWriterBase::flush() {
    sync();
}

DataFileSync DataFileWriterBase::makeSync() {
    boost::mt19937 random(static_cast<uint32_t>(time(nullptr)));
    DataFileSync sync;
    std::generate(sync.begin(), sync.end(), random);
    return sync;
}

typedef array<uint8_t, 4> Magic;
static Magic magic = {{'O', 'b', 'j', '\x01'}};

void DataFileWriterBase::writeHeader() {
    encoderPtr_->init(*stream_);
    avro::encode(*encoderPtr_, magic);
    avro::encode(*encoderPtr_, metadata_);
    avro::encode(*encoderPtr_, sync_);
    encoderPtr_->flush();
}

void DataFileWriterBase::setMetadata(const string &key, const string &value) {
    vector<uint8_t> v(value.size());
    copy(value.begin(), value.end(), v.begin());
    metadata_[key] = v;
}

DataFileReaderBase::DataFileReaderBase(const char *filename) : filename_(filename), stream_(fileSeekableInputStream(filename)),
                                                               decoder_(binaryDecoder()), objectCount_(0), eof_(false),
                                                               codec_(NULL_CODEC), blockStart_(-1), blockEnd_(-1) {
    readHeader();
}

DataFileReaderBase::DataFileReaderBase(std::unique_ptr<InputStream> inputStream) : stream_(std::move(inputStream)),
                                                                                   decoder_(binaryDecoder()), objectCount_(0), eof_(false), codec_(NULL_CODEC) {
    readHeader();
}

void DataFileReaderBase::init() {
    readerSchema_ = dataSchema_;
    dataDecoder_ = binaryDecoder();
    readDataBlock();
}

void DataFileReaderBase::init(const ValidSchema &readerSchema) {
    readerSchema_ = readerSchema;
    dataDecoder_ = (readerSchema_.toJson(true) != dataSchema_.toJson(true)) ? resolvingDecoder(dataSchema_, readerSchema_, binaryDecoder()) : binaryDecoder();
    readDataBlock();
}

static void drain(InputStream &in) {
    const uint8_t *p = nullptr;
    size_t n = 0;
    while (in.next(&p, &n))
        ;
}

char hex(unsigned int x) {
    return static_cast<char>(x + (x < 10 ? '0' : ('a' - 10)));
}

std::ostream &operator<<(std::ostream &os, const DataFileSync &s) {
    for (uint8_t i : s) {
        os << hex(i / 16) << hex(i % 16) << ' ';
    }
    os << std::endl;
    return os;
}

bool DataFileReaderBase::hasMore() {
    for (;;) {
        if (eof_) {
            return false;
        } else if (objectCount_ != 0) {
            return true;
        }

        dataDecoder_->init(*dataStream_);
        drain(*dataStream_);
        DataFileSync s;
        decoder_->init(*stream_);
        avro::decode(*decoder_, s);
        if (s != sync_) {
            throw Exception("Sync mismatch");
        }
        readDataBlock();
    }
}

class BoundedInputStream : public InputStream {
    InputStream &in_;
    size_t limit_;

    bool next(const uint8_t **data, size_t *len) final {
        if (limit_ != 0 && in_.next(data, len)) {
            if (*len > limit_) {
                in_.backup(*len - limit_);
                *len = limit_;
            }
            limit_ -= *len;
            return true;
        }
        return false;
    }

    void backup(size_t len) final {
        in_.backup(len);
        limit_ += len;
    }

    void skip(size_t len) final {
        if (len > limit_) {
            len = limit_;
        }
        in_.skip(len);
        limit_ -= len;
    }

    size_t byteCount() const final {
        return in_.byteCount();
    }

public:
    BoundedInputStream(InputStream &in, size_t limit) : in_(in), limit_(limit) {}
};

unique_ptr<InputStream> boundedInputStream(InputStream &in, size_t limit) {
    return unique_ptr<InputStream>(new BoundedInputStream(in, limit));
}

void DataFileReaderBase::readDataBlock() {
    decoder_->init(*stream_);
    blockStart_ = stream_->byteCount();
    const uint8_t *p = nullptr;
    size_t n = 0;
    if (!stream_->next(&p, &n)) {
        eof_ = true;
        return;
    }
    stream_->backup(n);
    avro::decode(*decoder_, objectCount_);
    int64_t byteCount;
    avro::decode(*decoder_, byteCount);
    decoder_->init(*stream_);
    blockEnd_ = stream_->byteCount() + byteCount;

    unique_ptr<InputStream> st = boundedInputStream(*stream_, static_cast<size_t>(byteCount));
    if (codec_ == NULL_CODEC) {
        dataDecoder_->init(*st);
        dataStream_ = std::move(st);
#ifdef SNAPPY_CODEC_AVAILABLE
    } else if (codec_ == SNAPPY_CODEC) {
        boost::crc_32_type crc;
        uint32_t checksum = 0;
        compressed_.clear();
        uncompressed.clear();
        const uint8_t *data;
        size_t len;
        while (st->next(&data, &len)) {
            compressed_.insert(compressed_.end(), data, data + len);
        }
        len = compressed_.size();
        if (len < 4)
            throw Exception("Cannot read compressed data, expected at least 4 bytes, got " + std::to_string(len));

        int b1 = compressed_[len - 4] & 0xFF;
        int b2 = compressed_[len - 3] & 0xFF;
        int b3 = compressed_[len - 2] & 0xFF;
        int b4 = compressed_[len - 1] & 0xFF;

        checksum = (b1 << 24) + (b2 << 16) + (b3 << 8) + (b4);
        if (!snappy::Uncompress(reinterpret_cast<const char *>(compressed_.data()),
                                len - 4, &uncompressed)) {
            throw Exception(
                "Snappy Compression reported an error when decompressing");
        }
        crc.process_bytes(uncompressed.c_str(), uncompressed.size());
        uint32_t c = crc();
        if (checksum != c) {
            throw Exception(
                "Checksum did not match for Snappy compression: Expected: {}, computed: {}",
                checksum, c);
        }
        os_.reset(new boost::iostreams::filtering_istream());
        os_->push(
            boost::iostreams::basic_array_source<char>(uncompressed.c_str(),
                                                       uncompressed.size()));
        std::unique_ptr<InputStream> in = istreamInputStream(*os_);

        dataDecoder_->init(*in);
        dataStream_ = std::move(in);
#endif
    } else {
        compressed_.clear();
        const uint8_t *data;
        size_t len;
        while (st->next(&data, &len)) {
            compressed_.insert(compressed_.end(), data, data + len);
        }
        os_.reset(new boost::iostreams::filtering_istream());
        os_->push(boost::iostreams::zlib_decompressor(get_zlib_params()));
        os_->push(boost::iostreams::basic_array_source<char>(
            compressed_.data(), compressed_.size()));

        std::unique_ptr<InputStream> in = nonSeekableIstreamInputStream(*os_);
        dataDecoder_->init(*in);
        dataStream_ = std::move(in);
    }
}

void DataFileReaderBase::close() {
}

static string toString(const vector<uint8_t> &v) {
    string result;
    result.resize(v.size());
    copy(v.begin(), v.end(), result.begin());
    return result;
}

static ValidSchema makeSchema(const vector<uint8_t> &v) {
    istringstream iss(toString(v));
    ValidSchema vs;
    compileJsonSchema(iss, vs);
    return vs;
}

void DataFileReaderBase::readHeader() {
    decoder_->init(*stream_);
    Magic m;
    avro::decode(*decoder_, m);
    if (magic != m) {
        throw Exception("Invalid data file. Magic does not match: "
                        + filename_);
    }
    avro::decode(*decoder_, metadata_);
    Metadata::const_iterator it = metadata_.find(AVRO_SCHEMA_KEY);
    if (it == metadata_.end()) {
        throw Exception("No schema in metadata");
    }

    dataSchema_ = makeSchema(it->second);
    if (!readerSchema_.root()) {
        readerSchema_ = dataSchema();
    }

    it = metadata_.find(AVRO_CODEC_KEY);
    if (it != metadata_.end() && toString(it->second) == AVRO_DEFLATE_CODEC) {
        codec_ = DEFLATE_CODEC;
#ifdef SNAPPY_CODEC_AVAILABLE
    } else if (it != metadata_.end()
               && toString(it->second) == AVRO_SNAPPY_CODEC) {
        codec_ = SNAPPY_CODEC;
#endif
    } else {
        codec_ = NULL_CODEC;
        if (it != metadata_.end() && toString(it->second) != AVRO_NULL_CODEC) {
            throw Exception("Unknown codec in data file: " + toString(it->second));
        }
    }

    avro::decode(*decoder_, sync_);
    decoder_->init(*stream_);
    blockStart_ = stream_->byteCount();
}

void DataFileReaderBase::doSeek(int64_t position) {
    if (auto *ss = dynamic_cast<SeekableInputStream *>(stream_.get())) {
        if (!eof_) {
            dataDecoder_->init(*dataStream_);
            drain(*dataStream_);
        }
        decoder_->init(*stream_);
        ss->seek(position);
        eof_ = false;
    } else {
        throw Exception("seek not supported on non-SeekableInputStream");
    }
}

void DataFileReaderBase::seek(int64_t position) {
    doSeek(position);
    readDataBlock();
}

void DataFileReaderBase::sync(int64_t position) {
    doSeek(position);
    DataFileSync sync_buffer;
    const uint8_t *p = nullptr;
    size_t n = 0;
    size_t i = 0;
    while (i < SyncSize) {
        if (n == 0 && !stream_->next(&p, &n)) {
            eof_ = true;
            return;
        }
        size_t len = std::min(SyncSize - i, n);
        memcpy(&sync_buffer[i], p, len);
        p += len;
        n -= len;
        i += len;
    }
    for (;;) {
        size_t j = 0;
        for (; j < SyncSize; ++j) {
            if (sync_[j] != sync_buffer[(i + j) % SyncSize]) {
                break;
            }
        }
        if (j == SyncSize) {
            // Found the sync marker!
            break;
        }
        if (n == 0 && !stream_->next(&p, &n)) {
            eof_ = true;
            return;
        }
        sync_buffer[i++ % SyncSize] = *p++;
        --n;
    }
    stream_->backup(n);
    readDataBlock();
}

bool DataFileReaderBase::pastSync(int64_t position) {
    return !hasMore() || blockStart_ >= position + SyncSize;
}

int64_t DataFileReaderBase::previousSync() const {
    return blockStart_;
}

} // namespace avro
