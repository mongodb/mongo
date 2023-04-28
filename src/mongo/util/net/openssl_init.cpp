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


#include "mongo/platform/basic.h"

#include "mongo/base/init.h"
#include "mongo/config.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/mutex.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/net/ssl_manager.h"
#include "mongo/util/net/ssl_options.h"
#include "mongo/util/scopeguard.h"

#include <memory>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <stack>
#include <vector>

#if OPENSSL_VERSION_NUMBER > 0x30000000L
#include <openssl/provider.h>
#endif

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork


namespace mongo {
namespace {

/**
 * Multithreaded Support for SSL.
 *
 * In order to allow OpenSSL to work in a multithreaded environment, you
 * may need to provide some callbacks for it to use for locking. The following code
 * sets up a vector of mutexes and provides a thread unique ID number.
 * The so-called SSLThreadInfo class encapsulates most of the logic required for
 * OpenSSL multithreaded support.
 *
 * OpenSSL before version 1.1.0 requires applications provide a callback which emits a thread
 * identifier. This ID is used to store thread specific ERR information. When a thread is
 * terminated, it must call ERR_remove_state or ERR_remove_thread_state. These functions may
 * themselves invoke the application provided callback. These IDs are stored in a hashtable with
 * a questionable hash function. They must be uniformly distributed to prevent collisions.
 */
class SSLThreadInfo {
public:
    static unsigned long getID() {
        /** A handle for the threadID resource. */
        struct ManagedId {
            ~ManagedId() {
                idManager().releaseID(id);
            }
            const unsigned long id = idManager().reserveID();
        };

        // The `guard` callback will cause an invocation of `getID`, so it must be destroyed first.
        static thread_local ManagedId managedId;
        static thread_local ScopeGuard guard([] { ERR_remove_state(0); });

        return managedId.id;
    }

    static void lockingCallback(int mode, int type, const char* file, int line) {
        if (mode & CRYPTO_LOCK) {
            mutexes()[type]->lock();
        } else {
            mutexes()[type]->unlock();
        }
    }

    static void init() {
        CRYPTO_set_id_callback(&SSLThreadInfo::getID);
        CRYPTO_set_locking_callback(&SSLThreadInfo::lockingCallback);

        while ((int)mutexes().size() < CRYPTO_num_locks()) {
            mutexes().emplace_back(std::make_unique<stdx::recursive_mutex>());
        }
    }

private:
    SSLThreadInfo() = delete;

    // Note: see SERVER-8734 for why we are using a recursive mutex here.
    // Once the deadlock fix in OpenSSL is incorporated into most distros of
    // Linux, this can be changed back to a nonrecursive mutex.
    static std::vector<std::unique_ptr<stdx::recursive_mutex>>& mutexes() {
        // Keep the static as a pointer to avoid it ever to be destroyed. It is referenced in the
        // CallErrRemoveState thread local above.
        static auto m = new std::vector<std::unique_ptr<stdx::recursive_mutex>>();
        return *m;
    }

    class ThreadIDManager {
    public:
        unsigned long reserveID() {
            stdx::unique_lock<Latch> lock(_idMutex);
            if (!_idLast.empty()) {
                unsigned long ret = _idLast.top();
                _idLast.pop();
                return ret;
            }
            return ++_idNext;
        }

        void releaseID(unsigned long id) {
            stdx::unique_lock<Latch> lock(_idMutex);
            _idLast.push(id);
        }

    private:
        // Machinery for producing IDs that are unique for the life of a thread.
        Mutex _idMutex =
            MONGO_MAKE_LATCH("ThreadIDManager::_idMutex");  // Protects _idNext and _idLast.
        unsigned long _idNext = 0;  // Stores the next thread ID to use, if none already allocated.
        std::stack<unsigned long, std::vector<unsigned long>>
            _idLast;  // Stores old thread IDs, for reuse.
    };

    static ThreadIDManager& idManager() {
        static auto& m = *new ThreadIDManager();
        return m;
    }
};

#if OPENSSL_VERSION_NUMBER > 0x30000000L
#define _SUPPORT_FIPS 1

OSSL_PROVIDER* fipsProvider;
OSSL_PROVIDER* baseProvider;

void initFIPS() {
    // OpenSSL 3 has a different FIPS design then previous OpenSSL. To load FIPS, we use the FIPS
    // algorithm provider which we load into the "default" library context.
    fipsProvider = OSSL_PROVIDER_load(NULL, "fips");
    if (fipsProvider == NULL) {
        LOGV2_FATAL_NOTRACE(
            7585801,
            "Failed to load OpenSSL 3 FIPS provider. OpenSSL was not compiled with FIPS support.",
            "error"_attr = SSLManagerInterface::getSSLErrorMessage(ERR_get_error()));
    }

    // Base provide has non-cryptographic algorihms (like encoding/decoding keys)
    baseProvider = OSSL_PROVIDER_load(NULL, "base");
    if (baseProvider == NULL) {
        LOGV2_FATAL_NOTRACE(7585802,
                            "Failed to load OpenSSL 3 Base provider",
                            "error"_attr =
                                SSLManagerInterface::getSSLErrorMessage(ERR_get_error()));
    }
}
#elif defined(MONGO_CONFIG_HAVE_FIPS_MODE_SET)

#define _SUPPORT_FIPS 1

void initFIPS() {
    int status = FIPS_mode_set(1);
    if (!status) {
        LOGV2_FATAL_NOTRACE(23173,
                            "Can't activate FIPS mode",
                            "error"_attr =
                                SSLManagerInterface::getSSLErrorMessage(ERR_get_error()));
    }
}
#endif

void setupFIPS() {
// Turn on FIPS mode if requested, OPENSSL_FIPS must be defined by the OpenSSL headers
#if defined(_SUPPORT_FIPS)
    initFIPS();
    LOGV2(23172, "FIPS 140-2 mode activated");
#else
    LOGV2_FATAL_NOTRACE(23174, "this version of mongodb was not compiled with FIPS support");
#endif
}

MONGO_INITIALIZER_GENERAL(SetupOpenSSL, ("default"), ("CryptographyInitialized"))
(InitializerContext*) {
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
    SSLThreadInfo::init();
}

}  // namespace
}  // namespace mongo
