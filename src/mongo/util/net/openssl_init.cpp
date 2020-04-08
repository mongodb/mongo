/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kNetwork

#include "mongo/platform/basic.h"

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <memory>
#include <vector>

#include "mongo/base/init.h"
#include "mongo/config.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/mutex.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/net/ssl_manager.h"
#include "mongo/util/net/ssl_options.h"
#include "mongo/util/scopeguard.h"

namespace mongo {
namespace {

/**
 * Multithreaded Support for SSL.
 *
 * In order to allow OpenSSL to work in a multithreaded environment, you
 * may need to provide some callbacks for it to use for locking. The following code
 * sets up a vector of mutexes and provides a thread unique ID number.
 *
 * OpenSSL before version 1.1.0 requires applications provide a callback which emits a thread
 * identifier. This ID is used to store thread specific ERR information. When a thread is
 * terminated, it must call ERR_remove_state or ERR_remove_thread_state. These functions may
 * themselves invoke the application provided callback. These IDs are stored in a hashtable with
 * a questionable hash function. They must be uniformly distributed to prevent collisions.
 */

class ThreadIDManager {
public:
    ~ThreadIDManager() = delete;  // Cannot die.

    static ThreadIDManager& instance() {
        static auto& m = *new ThreadIDManager();
        return m;
    }

    unsigned long reserveID() {
        auto lock = stdx::lock_guard(_idMutex);
        if (!_idPool.empty()) {
            unsigned long ret = _idPool.back();
            _idPool.pop_back();
            return ret;
        }
        return ++_idNext;
    }

    void releaseID(unsigned long id) {
        auto lock = stdx::lock_guard(_idMutex);
        _idPool.push_back(id);
    }

private:
    // Machinery for producing IDs that are unique for the life of a thread.
    Mutex _idMutex = MONGO_MAKE_LATCH("ThreadIDManager::_idMutex");  // Guards _idNext, _idLast.
    unsigned long _idNext = 0;  // Stores the next thread ID to use, if none already allocated.
    std::vector<unsigned long> _idPool;  // Stack of old thread IDs for reuse.
};

/** A handle for the threadID resource. */
struct ManagedId {
    ~ManagedId() {
        ThreadIDManager::instance().releaseID(id);
    }
    const unsigned long id = ThreadIDManager::instance().reserveID();
};

unsigned long getID() {
    // The `guard` callback will cause an invocation of `getID`, so it must be destroyed first.
    thread_local ManagedId managedId{};
    thread_local auto guard = makeGuard([]{ ERR_remove_state(0); });
    return managedId.id;
}

void lockingCallback(int mode, int type, const char* file, int line) {
    // Note: see SERVER-8734 for why we are using a recursive mutex here.
    // Once the deadlock fix in OpenSSL is incorporated into most distros of
    // Linux, this can be changed back to a nonrecursive mutex.
    static auto& mutexes = *new std::vector<stdx::recursive_mutex>(CRYPTO_num_locks());
    auto& m = mutexes[type];
    if (mode & CRYPTO_LOCK) {
        m.lock();
    } else {
        m.unlock();
    }
}

void setupFIPS() {
// Turn on FIPS mode if requested, OPENSSL_FIPS must be defined by the OpenSSL headers
#if defined(MONGO_CONFIG_HAVE_FIPS_MODE_SET)
    int status = FIPS_mode_set(1);
    if (!status) {
        LOGV2_FATAL(23173,
                    "can't activate FIPS mode: {error}",
                    "Can't activate FIPS mode",
                    "error"_attr = SSLManagerInterface::getSSLErrorMessage(ERR_get_error()));
        fassertFailedNoTrace(16703);
    }
    LOGV2(23172, "FIPS 140-2 mode activated");
#else
    LOGV2_FATAL(23174, "this version of mongodb was not compiled with FIPS support");
    fassertFailedNoTrace(17089);
#endif
}

MONGO_INITIALIZER(SetupOpenSSL)(InitializerContext*) {
    SSL_library_init();
    SSL_load_error_strings();
    ERR_load_crypto_strings();

    if (sslGlobalParams.sslFIPSMode) {
        setupFIPS();
    }

    // Add all digests and ciphers to OpenSSL's internal table
    // so that encryption/decryption is backwards compatible
    OpenSSL_add_all_algorithms();

    // Setup OpenSSL multithreading callbacks and mutexes
    CRYPTO_set_id_callback(&getID);
    CRYPTO_set_locking_callback(&lockingCallback);

    return Status::OK();
}

}  // namespace
}  // namespace mongo
