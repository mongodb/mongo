// @file compress.cpp

#include "../third_party/snappy/snappy.h"
#include "compress.h"
#include <string>
#include <assert.h>

namespace mongo {

    size_t compress(const char* input, size_t input_length, std::string* output) { 
        return snappy::Compress(input, input_length, output);
    }

    bool uncompress(const char* compressed, size_t compressed_length, std::string* uncompressed) { 
        return snappy::Uncompress(compressed, compressed_length, uncompressed);
    }

    struct CTest { 
        CTest() { 
            const char * c = "this is a test";
            std::string s;
            size_t len = compress(c, strlen(c)+1, &s);

            std::string out;
            bool ok = uncompress(s.c_str(), s.size(), &out);
            assert(ok);
            assert( strcmp(out.c_str(), c) == 0 );
        }
    } ctest;

}
