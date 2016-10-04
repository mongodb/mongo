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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include "mongo/platform/random.h"

#include <string.h>

#ifdef _WIN32
#include <bcrypt.h>
#else
#include <errno.h>
#endif

#define _CRT_RAND_S
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>

#include "mongo/stdx/memory.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"

namespace mongo {

// ---- PseudoRandom  -----

uint32_t PseudoRandom::nextUInt32() {
    uint32_t t = _x ^ (_x << 11);
    _x = _y;
    _y = _z;
    _z = _w;
    return _w = _w ^ (_w >> 19) ^ (t ^ (t >> 8));
}

namespace {
const uint32_t default_y = 362436069;
const uint32_t default_z = 521288629;
const uint32_t default_w = 88675123;
}  // namespace

PseudoRandom::PseudoRandom(uint32_t seed) {
    _x = seed;
    _y = default_y;
    _z = default_z;
    _w = default_w;
}

PseudoRandom::PseudoRandom(int32_t seed) : PseudoRandom(static_cast<uint32_t>(seed)) {}

PseudoRandom::PseudoRandom(int64_t seed)
    : PseudoRandom(static_cast<uint32_t>(seed >> 32) ^ static_cast<uint32_t>(seed)) {}

int32_t PseudoRandom::nextInt32() {
    return nextUInt32();
}

int64_t PseudoRandom::nextInt64() {
    uint64_t a = nextUInt32();
    uint64_t b = nextUInt32();
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
public:
    WinSecureRandom() {
        auto ntstatus = ::BCryptOpenAlgorithmProvider(
            &_algHandle, BCRYPT_RNG_ALGORITHM, MS_PRIMITIVE_PROVIDER, 0);
        if (ntstatus != STATUS_SUCCESS) {
            error() << "Failed to open crypto algorithm provider while creating secure random "
                       "object; NTSTATUS: "
                    << ntstatus;
            fassertFailed(28815);
        }
    }

    virtual ~WinSecureRandom() {
        auto ntstatus = ::BCryptCloseAlgorithmProvider(_algHandle, 0);
        if (ntstatus != STATUS_SUCCESS) {
            warning() << "Failed to close crypto algorithm provider destroying secure random "
                         "object; NTSTATUS: "
                      << ntstatus;
        }
    }

    int64_t nextInt64() {
        int64_t value;
        auto ntstatus =
            ::BCryptGenRandom(_algHandle, reinterpret_cast<PUCHAR>(&value), sizeof(value), 0);
        if (ntstatus != STATUS_SUCCESS) {
            error() << "Failed to generate random number from secure random object; NTSTATUS: "
                    << ntstatus;
            fassertFailed(28814);
        }
        return value;
    }

private:
    BCRYPT_ALG_HANDLE _algHandle;
};

SecureRandom* SecureRandom::create() {
    return new WinSecureRandom();
}

#elif defined(__linux__) || defined(__sun) || defined(__APPLE__) || defined(__FreeBSD__)

class InputStreamSecureRandom : public SecureRandom {
public:
    InputStreamSecureRandom(const char* fn) {
        _in = stdx::make_unique<std::ifstream>(fn, std::ios::binary | std::ios::in);
        if (!_in->is_open()) {
            error() << "cannot open " << fn << " " << strerror(errno);
            fassertFailed(28839);
        }
    }

    int64_t nextInt64() {
        int64_t r;
        _in->read(reinterpret_cast<char*>(&r), sizeof(r));
        if (_in->fail()) {
            error() << "InputStreamSecureRandom failed to generate random bytes";
            fassertFailed(28840);
        }
        return r;
    }

private:
    std::unique_ptr<std::ifstream> _in;
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
}  // namespace mongo
