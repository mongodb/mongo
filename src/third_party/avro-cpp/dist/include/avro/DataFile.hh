/*
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

#ifndef avro_DataFile_hh__
#define avro_DataFile_hh__

#include "Config.hh"
#include "Encoder.hh"
#include "Specific.hh"
#include "Stream.hh"
#include "ValidSchema.hh"
#include "buffer/Buffer.hh"

#include <map>
#include <string>
#include <vector>

#include "array"
#include "boost/utility.hpp"
#include <boost/iostreams/filtering_stream.hpp>

namespace avro {

/** Specify type of compression to use when writing data files. */
enum Codec {
    NULL_CODEC,
    DEFLATE_CODEC,

#ifdef SNAPPY_CODEC_AVAILABLE
    SNAPPY_CODEC
#endif

};

const int SyncSize = 16;
/**
 * The sync value.
 */
typedef std::array<uint8_t, SyncSize> DataFileSync;

/**
 * Type-independent portion of DataFileWriter.
 *  At any given point in time, at most one file can be written using
 *  this object.
 */
class AVRO_DECL DataFileWriterBase : boost::noncopyable {
    const std::string filename_;
    const ValidSchema schema_;
    const EncoderPtr encoderPtr_;
    const size_t syncInterval_;
    Codec codec_;

    std::unique_ptr<OutputStream> stream_;
    std::unique_ptr<OutputStream> buffer_;
    const DataFileSync sync_;
    int64_t objectCount_;

    typedef std::map<std::string, std::vector<uint8_t>> Metadata;

    Metadata metadata_;
    int64_t lastSync_;

    static std::unique_ptr<OutputStream> makeStream(const char *filename);
    static DataFileSync makeSync();

    void writeHeader();
    void setMetadata(const std::string &key, const std::string &value);

    /**
     * Generates a sync marker in the file.
     */
    void sync();

    /**
     * Shared constructor portion since we aren't using C++11
     */
    void init(const ValidSchema &schema, size_t syncInterval, const Codec &codec);

public:
    /**
     * Returns the current encoder for this writer.
     */
    Encoder &encoder() const { return *encoderPtr_; }

    /**
     * Returns true if the buffer has sufficient data for a sync to be
     * inserted.
     */
    void syncIfNeeded();

    /**
     * Returns the byte offset (within the current file) of the start of the current block being written.
     */
    uint64_t getCurrentBlockStart() const;

    /**
     * Increments the object count.
     */
    void incr() {
        ++objectCount_;
    }
    /**
     * Constructs a data file writer with the given sync interval and name.
     */
    DataFileWriterBase(const char *filename, const ValidSchema &schema,
                       size_t syncInterval, Codec codec = NULL_CODEC);
    DataFileWriterBase(std::unique_ptr<OutputStream> outputStream,
                       const ValidSchema &schema, size_t syncInterval, Codec codec);

    ~DataFileWriterBase();
    /**
     * Closes the current file. Once closed this datafile object cannot be
     * used for writing any more.
     */
    void close();

    /**
     * Returns the schema for this data file.
     */
    const ValidSchema &schema() const { return schema_; }

    /**
     * Flushes any unwritten data into the file.
     */
    void flush();
};

/**
 *  An Avro datafile that can store objects of type T.
 */
template<typename T>
class DataFileWriter : boost::noncopyable {
    std::unique_ptr<DataFileWriterBase> base_;

public:
    /**
     * Constructs a new data file.
     */
    DataFileWriter(const char *filename, const ValidSchema &schema,
                   size_t syncInterval = 16 * 1024, Codec codec = NULL_CODEC) : base_(new DataFileWriterBase(filename, schema, syncInterval, codec)) {}

    DataFileWriter(std::unique_ptr<OutputStream> outputStream, const ValidSchema &schema,
                   size_t syncInterval = 16 * 1024, Codec codec = NULL_CODEC) : base_(new DataFileWriterBase(std::move(outputStream), schema, syncInterval, codec)) {}

    /**
     * Writes the given piece of data into the file.
     */
    void write(const T &datum) {
        base_->syncIfNeeded();
        avro::encode(base_->encoder(), datum);
        base_->incr();
    }

    /**
     *  Returns the byte offset (within the current file) of the start of the current block being written.
     */
    uint64_t getCurrentBlockStart() { return base_->getCurrentBlockStart(); }

    /**
     * Closes the current file. Once closed this datafile object cannot be
     * used for writing any more.
     */
    void close() { base_->close(); }

    /**
     * Returns the schema for this data file.
     */
    const ValidSchema &schema() const { return base_->schema(); }

    /**
     * Flushes any unwritten data into the file.
     */
    void flush() { base_->flush(); }
};

/**
 * The type independent portion of reader.
 */
class AVRO_DECL DataFileReaderBase : boost::noncopyable {
    const std::string filename_;
    const std::unique_ptr<InputStream> stream_;
    const DecoderPtr decoder_;
    int64_t objectCount_;
    bool eof_;
    Codec codec_;
    int64_t blockStart_{};
    int64_t blockEnd_{};

    ValidSchema readerSchema_;
    ValidSchema dataSchema_;
    DecoderPtr dataDecoder_;
    std::unique_ptr<InputStream> dataStream_;
    typedef std::map<std::string, std::vector<uint8_t>> Metadata;

    Metadata metadata_;
    DataFileSync sync_{};

    // for compressed buffer
    std::unique_ptr<boost::iostreams::filtering_istream> os_;
    std::vector<char> compressed_;
    std::string uncompressed;
    void readHeader();

    void readDataBlock();
    void doSeek(int64_t position);

public:
    /**
     * Returns the current decoder for this reader.
     */
    Decoder &decoder() { return *dataDecoder_; }

    /**
     * Returns true if and only if there is more to read.
     */
    bool hasMore();

    /**
     * Decrements the number of objects yet to read.
     */
    void decr() { --objectCount_; }

    /**
     * Constructs the reader for the given file and the reader is
     * expected to use the schema that is used with data.
     * This function should be called exactly once after constructing
     * the DataFileReaderBase object.
     */
    explicit DataFileReaderBase(const char *filename);

    explicit DataFileReaderBase(std::unique_ptr<InputStream> inputStream);

    /**
     * Initializes the reader so that the reader and writer schemas
     * are the same.
     */
    void init();

    /**
     * Initializes the reader to read objects according to the given
     * schema. This gives an opportunity for the reader to see the schema
     * in the data file before deciding the right schema to use for reading.
     * This must be called exactly once after constructing the
     * DataFileReaderBase object.
     */
    void init(const ValidSchema &readerSchema);

    /**
     * Returns the schema for this object.
     */
    const ValidSchema &readerSchema() { return readerSchema_; }

    /**
     * Returns the schema stored with the data file.
     */
    const ValidSchema &dataSchema() { return dataSchema_; }

    /**
     * Closes the reader. No further operation is possible on this reader.
     */
    void close();

    /**
     * Move to a specific, known synchronization point, for example one returned
     * from tell() after sync().
     */
    void seek(int64_t position);

    /**
     * Move to the next synchronization point after a position. To process a
     * range of file entries, call this with the starting position, then check
     * pastSync() with the end point before each use of decoder().
     */
    void sync(int64_t position);

    /**
     * Return true if past the next synchronization point after a position.
     */
    bool pastSync(int64_t position);

    /**
     * Return the last synchronization point before our current position.
     */
    int64_t previousSync() const;
};

/**
 * Reads the contents of data file one after another.
 */
template<typename T>
class DataFileReader : boost::noncopyable {
    std::unique_ptr<DataFileReaderBase> base_;

public:
    /**
     * Constructs the reader for the given file and the reader is
     * expected to use the given schema.
     */
    DataFileReader(const char *filename, const ValidSchema &readerSchema) : base_(new DataFileReaderBase(filename)) {
        base_->init(readerSchema);
    }

    DataFileReader(std::unique_ptr<InputStream> inputStream, const ValidSchema &readerSchema) : base_(new DataFileReaderBase(std::move(inputStream))) {
        base_->init(readerSchema);
    }

    /**
     * Constructs the reader for the given file and the reader is
     * expected to use the schema that is used with data.
     */
    explicit DataFileReader(const char *filename) : base_(new DataFileReaderBase(filename)) {
        base_->init();
    }

    explicit DataFileReader(std::unique_ptr<InputStream> inputStream) : base_(new DataFileReaderBase(std::move(inputStream))) {
        base_->init();
    }

    /**
     * Constructs a reader using the reader base. This form of constructor
     * allows the user to examine the schema of a given file and then
     * decide to use the right type of data to be deserialize. Without this
     * the user must know the type of data for the template _before_
     * he knows the schema within the file.
     * The schema present in the data file will be used for reading
     * from this reader.
     */
    explicit DataFileReader(std::unique_ptr<DataFileReaderBase> base) : base_(std::move(base)) {
        base_->init();
    }

    /**
     * Constructs a reader using the reader base. This form of constructor
     * allows the user to examine the schema of a given file and then
     * decide to use the right type of data to be deserialize. Without this
     * the user must know the type of data for the template _before_
     * he knows the schema within the file.
     * The argument readerSchema will be used for reading
     * from this reader.
     */
    DataFileReader(std::unique_ptr<DataFileReaderBase> base,
                   const ValidSchema &readerSchema) : base_(std::move(base)) {
        base_->init(readerSchema);
    }

    /**
     * Reads the next entry from the data file.
     * \return true if an object has been successfully read into \p datum and
     * false if there are no more entries in the file.
     */
    bool read(T &datum) {
        if (base_->hasMore()) {
            base_->decr();
            avro::decode(base_->decoder(), datum);
            return true;
        }
        return false;
    }

    /**
     * Returns the schema for this object.
     */
    const ValidSchema &readerSchema() { return base_->readerSchema(); }

    /**
     * Returns the schema stored with the data file.
     */
    const ValidSchema &dataSchema() { return base_->dataSchema(); }

    /**
     * Closes the reader. No further operation is possible on this reader.
     */
    void close() { return base_->close(); }

    /**
     * Move to a specific, known synchronization point, for example one returned
     * from previousSync().
     */
    void seek(int64_t position) { base_->seek(position); }

    /**
     * Move to the next synchronization point after a position. To process a
     * range of file entries, call this with the starting position, then check
     * pastSync() with the end point before each call to read().
     */
    void sync(int64_t position) { base_->sync(position); }

    /**
     * Return true if past the next synchronization point after a position.
     */
    bool pastSync(int64_t position) { return base_->pastSync(position); }

    /**
     * Return the last synchronization point before our current position.
     */
    int64_t previousSync() { return base_->previousSync(); }
};

} // namespace avro
#endif
