// @file compress.cpp

/**
*    Copyright (C) 2012 10gen Inc.
*
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "mongo/util/compress.h"

#include "snappy.h"

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
