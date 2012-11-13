// random.cpp

/*    Copyright 2012 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#include "mongo/platform/basic.h"

#include "mongo/platform/random.h"

#include <cstdlib>
#include <iostream>
#include <fstream>
#include <stdio.h>
#include <string.h>

#ifndef _WIN32
#include <errno.h>
#endif

namespace mongo {

    // ---- PseudoRandom  -----

#ifdef _WIN32
#pragma warning( disable : 4715 ) // not all control paths return a value
    int32_t PseudoRandom::nextInt32() {
        if ( rand_s(&_seed) == 0 ) {
            // SUCCESS
            return _seed;
        }
        abort();
    }

#else
    int32_t PseudoRandom::nextInt32() {
        return rand_r( &_seed );
    }
#endif
    PseudoRandom::PseudoRandom( int64_t seed ) {
        _seed = static_cast<uint32_t>( seed );
    }

    int64_t PseudoRandom::nextInt64() {
        int64_t a = nextInt32();
        int64_t b = nextInt32();
        return ( a << 32 ) | b;
    }

    // --- SecureRandom ----

    SecureRandom::~SecureRandom() {
    }

#ifdef _WIN32
    class WinSecureRandom : public SecureRandom {
        virtual ~WinSecureRandom(){}
        int64_t nextInt64() {
            uint32_t a, b;
            if ( rand_s(&a) ) {
                abort();
            }
            if ( rand_s(&b) ) {
                abort();
            }
            return ( static_cast<int64_t>(a) << 32 ) | b;
        }
    };

    SecureRandom* SecureRandom::create() {
        return new WinSecureRandom();
    }

#elif defined(__linux__) || defined(__sunos__) || defined(__APPLE__)

    class InputStreamSecureRandom : public SecureRandom {
    public:
        InputStreamSecureRandom( const char* fn ) {
            _in = new std::ifstream( fn, std::ios::binary | std::ios::in );
            if ( ! _in->is_open() ) {
                std::cerr << "can't open " << fn << " " << strerror(errno) << std::endl;
                abort();
            }
        }

        ~InputStreamSecureRandom() {
            delete _in;
        }

        int64_t nextInt64() {
            int64_t r;
            _in->read( reinterpret_cast<char*>( &r ), sizeof(r) );
            if ( _in->fail() ) {
                abort();
            }
            return r;
        }

    private:
        std::ifstream* _in;
    };

    SecureRandom* SecureRandom::create() {
        return new InputStreamSecureRandom( "/dev/urandom" );
    }

#else
    class SRandSecureRandom : public SecureRandom {
    public:
        SRandSecureRandom() {
            srandomdev();
        }

        int64_t nextInt64() {
            long a, b;
            a = random();
            b = random();
            return ( static_cast<int64_t>(a) << 32 ) | b;
        }
    };

    SecureRandom* SecureRandom::create() {
        return new SRandSecureRandom();
    }


#endif

}
