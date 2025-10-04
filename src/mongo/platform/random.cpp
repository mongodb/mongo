/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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


#include <array>
#include <cstring>
#include <fstream>  // IWYU pragma: keep
#include <memory>

#ifdef _WIN32
#include <bcrypt.h>
#else
#include <cerrno>

#include <fcntl.h>
#endif

#define _CRT_RAND_S

#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/logv2/log.h"
#include "mongo/platform/random.h"
#include "mongo/util/assert_util.h"

#if defined(MONGO_CONFIG_HAVE_HEADER_UNISTD_H)
#include <unistd.h>
#endif

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault


#ifdef _WIN32
#define SECURE_RANDOM_BCRYPT
#elif defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__) || defined(__EMSCRIPTEN__)
#define SECURE_RANDOM_URANDOM
#elif defined(__OpenBSD__)
#define SECURE_RANDOM_ARCFOUR
#else
#error "Must implement SecureRandom for platform"
#endif

namespace mongo {

namespace {

template <size_t Bytes>
struct Buffer {
    static constexpr size_t kArraySize = Bytes / sizeof(uint64_t);

    uint64_t pop() {
        return arr[--avail];
    }
    uint8_t* fillPtr() {
        return reinterpret_cast<uint8_t*>(arr.data() + avail);
    }
    size_t fillSize() {
        return sizeof(uint64_t) * (arr.size() - avail);
    }
    void setFilled() {
        avail = arr.size();
    }

    std::array<uint64_t, kArraySize> arr;
    size_t avail = 0;
};

#if defined(SECURE_RANDOM_BCRYPT)
class Source {
public:
    Source() {
        auto ntstatus = ::BCryptOpenAlgorithmProvider(
            &_algHandle, BCRYPT_RNG_ALGORITHM, MS_PRIMITIVE_PROVIDER, 0);
        if (ntstatus != STATUS_SUCCESS) {
            LOGV2_ERROR(23822,
                        "Failed to open crypto algorithm provider while creating secure random "
                        "object; NTSTATUS: {ntstatus}",
                        "ntstatus"_attr = ntstatus);
            fassertFailed(28815);
        }
    }

    ~Source() {
        auto ntstatus = ::BCryptCloseAlgorithmProvider(_algHandle, 0);
        if (ntstatus != STATUS_SUCCESS) {
            LOGV2_WARNING(23821,
                          "Failed to close crypto algorithm provider destroying secure random "
                          "object; NTSTATUS: {ntstatus}",
                          "ntstatus"_attr = ntstatus);
        }
    }

    size_t refill(uint8_t* buf, size_t n) {
        auto ntstatus = ::BCryptGenRandom(_algHandle, reinterpret_cast<PUCHAR>(buf), n, 0);
        if (ntstatus != STATUS_SUCCESS) {
            LOGV2_ERROR(
                23823,
                "Failed to generate random number from secure random object; NTSTATUS: {ntstatus}",
                "ntstatus"_attr = ntstatus);
            fassertFailed(28814);
        }
        return n;
    }

private:
    BCRYPT_ALG_HANDLE _algHandle;
};
#endif  // SECURE_RANDOM_BCRYPT

#if defined(SECURE_RANDOM_URANDOM)
class Source {
public:
    size_t refill(uint8_t* buf, size_t n) {
        size_t i = 0;
        while (i < n) {
            ssize_t r;
            while ((r = read(sharedFd(), buf + i, n - i)) == -1) {
                if (errno == EINTR) {
                    continue;
                } else {
                    auto errSave = errno;
                    LOGV2_ERROR(23824,
                                "SecureRandom: read `{kFn}`: {strerror_errSave}",
                                "kFn"_attr = kFn,
                                "strerror_errSave"_attr = strerror(errSave));
                    fassertFailed(28840);
                }
            }
            i += r;
        }
        return i;
    }

private:
    static constexpr const char* kFn = "/dev/urandom";
    static int sharedFd() {
        // Retain the urandom fd forever.
        // Kernel ensures that concurrent `read` calls don't mingle their data.
        // http://lkml.iu.edu//hypermail/linux/kernel/0412.1/0181.html
        static const int fd = [] {
            int f;
            while ((f = open(kFn, 0)) == -1) {
                if (errno == EINTR) {
                    continue;
                } else {
                    auto errSave = errno;
                    LOGV2_ERROR(23825,
                                "SecureRandom: open `{kFn}`: {strerror_errSave}",
                                "kFn"_attr = kFn,
                                "strerror_errSave"_attr = strerror(errSave));
                    fassertFailed(28839);
                }
            }
            return f;
        }();
        return fd;
    }
};
#endif  // SECURE_RANDOM_URANDOM

#if defined(SECURE_RANDOM_ARCFOUR)
class Source {
public:
    size_t refill(uint8_t* buf, size_t n) {
        arc4random_buf(buf, n);
        return n;
    }
};
#endif  // SECURE_RANDOM_ARCFOUR

}  // namespace

class SecureUrbg::State {
public:
    uint64_t get() {
        if (!_buffer.avail) {
            size_t n = _source.refill(_buffer.fillPtr(), _buffer.fillSize());
            _buffer.avail += n / sizeof(uint64_t);
        }
        return _buffer.pop();
    }

private:
    Source _source;
    Buffer<4096> _buffer;
};

SecureUrbg::SecureUrbg() : _state{std::make_unique<State>()} {}

SecureUrbg::~SecureUrbg() = default;

uint64_t SecureUrbg::operator()() {
    return _state->get();
}

}  // namespace mongo
