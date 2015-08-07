// random.cpp

/*    Copyright 2012 10gen Inc.
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
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/platform/random.h"

#include <stdio.h>
#include <string.h>

#ifndef _WIN32
#include <errno.h>
#endif

#define _CRT_RAND_S
#include <cstdlib>
#include <iostream>
#include <fstream>
#include <limits>

#include "mongo/platform/basic.h"

namespace mongo {

// ---- PseudoRandom  -----

int32_t PseudoRandom::nextInt32() {
    int32_t t = _x ^ (_x << 11);
    _x = _y;
    _y = _z;
    _z = _w;
    return _w = _w ^ (_w >> 19) ^ (t ^ (t >> 8));
}

namespace {
const int32_t default_y = 362436069;
const int32_t default_z = 521288629;
const int32_t default_w = 88675123;
}

PseudoRandom::PseudoRandom(int32_t seed) {
    _x = seed;
    _y = default_y;
    _z = default_z;
    _w = default_w;
}


PseudoRandom::PseudoRandom(uint32_t seed) {
    _x = static_cast<int32_t>(seed);
    _y = default_y;
    _z = default_z;
    _w = default_w;
}


PseudoRandom::PseudoRandom(int64_t seed) {
    int32_t high = seed >> 32;
    int32_t low = seed & 0xFFFFFFFF;

    _x = high ^ low;
    _y = default_y;
    _z = default_z;
    _w = default_w;
}

int64_t PseudoRandom::nextInt64() {
    int64_t a = nextInt32();
    int64_t b = nextInt32();
    return (a << 32) | b;
}

double PseudoRandom::nextCanonicalDouble() {
    double result;
    do {
        auto generated = static_cast<uint64_t>(nextInt64());
        result = static_cast<double>(generated) / std::numeric_limits<uint64_t>::max();
    } while (result == 1.0);
    return result;
}

// --- SecureRandom ----

SecureRandom::~SecureRandom() {}

#ifdef _WIN32
class WinSecureRandom : public SecureRandom {
    virtual ~WinSecureRandom() {}
    int64_t nextInt64() {
        uint32_t a, b;
        if (rand_s(&a)) {
            abort();
        }
        if (rand_s(&b)) {
            abort();
        }
        return (static_cast<int64_t>(a) << 32) | b;
    }
};

SecureRandom* SecureRandom::create() {
    return new WinSecureRandom();
}

#elif defined(__linux__) || defined(__sun) || defined(__APPLE__) || defined(__FreeBSD__)

class InputStreamSecureRandom : public SecureRandom {
public:
    InputStreamSecureRandom(const char* fn) {
        _in = new std::ifstream(fn, std::ios::binary | std::ios::in);
        if (!_in->is_open()) {
            std::cerr << "can't open " << fn << " " << strerror(errno) << std::endl;
            abort();
        }
    }

    ~InputStreamSecureRandom() {
        delete _in;
    }

    int64_t nextInt64() {
        int64_t r;
        _in->read(reinterpret_cast<char*>(&r), sizeof(r));
        if (_in->fail()) {
            abort();
        }
        return r;
    }

private:
    std::ifstream* _in;
};

SecureRandom* SecureRandom::create() {
    return new InputStreamSecureRandom("/dev/urandom");
}

#elif defined(__OpenBSD__)

class Arc4SecureRandom : public SecureRandom {
public:
    int64_t nextInt64() {
        int64_t value;
        arc4random_buf(&value, sizeof(value));
        return value;
    }
};

SecureRandom* SecureRandom::create() {
    return new Arc4SecureRandom();
}

#else

#error Must implement SecureRandom for platform

#endif
}
