// @file compress.cpp

#include "mongo/util/compress.h"

#include "third_party/snappy/snappy.h"

namespace mongo {

    void rawCompress(const char* input,
        size_t input_length,
        char* compressed,
        size_t* compressed_length)
    {
        snappy::RawCompress(input, input_length, compressed, compressed_length);
    }

    size_t maxCompressedLength(size_t source_len) {
        return snappy::MaxCompressedLength(source_len);
    }

    size_t compress(const char* input, size_t input_length, std::string* output) {
        return snappy::Compress(input, input_length, output);
    }

    bool uncompress(const char* compressed, size_t compressed_length, std::string* uncompressed) {
        return snappy::Uncompress(compressed, compressed_length, uncompressed);
    }

}
