/*    Copyright 2013 10gen Inc.
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

#include "gperftools/malloc_extension.h"

#include "mongo/base/init.h"
#include "mongo/db/commands/server_status.h"
#include "mongo/util/concurrency/synchronization.h"
#include "mongo/util/log.h"
#include "mongo/util/net/listen.h"

namespace mongo {

namespace {
    // If many clients are used, the per-thread caches become smaller and chances of
    // rebalancing of free space during critical sections increases. In such situations,
    // it is better to release memory when it is likely the thread will be blocked for
    // a long time.
    const int kManyClients = 40;

    boost::mutex tcmallocCleanupLock;

    /**
     *  Callback to allow TCMalloc to release freed memory to the central list at
     *  favorable times. Ideally would do some milder cleanup or scavenge...
     */
    void threadStateChange() {
        if (Listener::globalTicketHolder.used() <= kManyClients) {
            return;
        }

#if MONGO_HAVE_GPERFTOOLS_GET_THREAD_CACHE_SIZE
        size_t threadCacheSizeBytes = MallocExtension::instance()->GetThreadCacheSize();

        static const size_t kMaxThreadCacheSizeBytes = 0x10000;
        if (threadCacheSizeBytes < kMaxThreadCacheSizeBytes) {
            // This number was chosen a bit magically.
            // At 1000 threads and the current (64mb) thread local cache size, we're "full".
            // So we may want this number to scale with the number of current clients.
            return;
        }

        LOG(1) << "thread over memory limit, cleaning up, current: "
               << (threadCacheSizeBytes/1024) << "k";
#endif

        // We synchronize as the tcmalloc central list uses a spinlock, and we can cause a really
        // terrible runaway if we're not careful.
        boost::lock_guard<boost::mutex> lk(tcmallocCleanupLock);
        MallocExtension::instance()->MarkThreadIdle();
        MallocExtension::instance()->MarkThreadBusy();
    }

    // Register threadStateChange callback
    MONGO_INITIALIZER(TCMallocThreadIdleListener)(InitializerContext*) {
        registerThreadIdleCallback(&threadStateChange);
        return Status::OK();
    }

    class TCMallocServerStatusSection : public ServerStatusSection {
    public:

        TCMallocServerStatusSection() : ServerStatusSection("tcmalloc") {}
        virtual bool includeByDefault() const { return false; }

        virtual BSONObj generateSection(OperationContext* txn,
                                        const BSONElement& configElement) const {

            BSONObjBuilder builder;

            // For a list of properties see the "Generic Tcmalloc Status" section of
            // http://google-perftools.googlecode.com/svn/trunk/doc/tcmalloc.html and
            // http://code.google.com/p/gperftools/source/browse/src/gperftools/malloc_extension.h
            {
                BSONObjBuilder sub(builder.subobjStart("generic"));
                appendNumericPropertyIfAvailable(sub,    "current_allocated_bytes",
                                                 "generic.current_allocated_bytes");
                appendNumericPropertyIfAvailable(sub,    "heap_size",
                                                 "generic.heap_size");
            }
            {
                BSONObjBuilder sub(builder.subobjStart("tcmalloc"));
                appendNumericPropertyIfAvailable(sub,     "pageheap_free_bytes",
                                                 "tcmalloc.pageheap_free_bytes");
                appendNumericPropertyIfAvailable(sub,     "pageheap_unmapped_bytes",
                                                 "tcmalloc.pageheap_unmapped_bytes");
                appendNumericPropertyIfAvailable(sub,     "max_total_thread_cache_bytes",
                                                 "tcmalloc.max_total_thread_cache_bytes");
                appendNumericPropertyIfAvailable(sub,     "current_total_thread_cache_bytes",
                                                 "tcmalloc.current_total_thread_cache_bytes");
                // Not including tcmalloc.slack_bytes since it is deprecated.

                appendNumericPropertyIfAvailable(sub,     "central_cache_free_bytes",
                                                 "tcmalloc.central_cache_free_bytes");
                appendNumericPropertyIfAvailable(sub,     "transfer_cache_free_bytes",
                                                 "tcmalloc.transfer_cache_free_bytes");
                appendNumericPropertyIfAvailable(sub,     "thread_cache_free_bytes",
                                                 "tcmalloc.thread_cache_free_bytes");
                appendNumericPropertyIfAvailable(sub,     "aggressive_memory_decommit",
                                                 "tcmalloc.aggressive_memory_decommit");
            }

            char buffer[4096];
            MallocExtension::instance()->GetStats(buffer, sizeof(buffer));
            builder.append("formattedString", buffer);

            return builder.obj();
        }

    private:
        static void appendNumericPropertyIfAvailable(BSONObjBuilder& builder,
                                                     StringData bsonName,
                                                     const char* property) {
            size_t value;
            if (MallocExtension::instance()->GetNumericProperty(property, &value))
                builder.appendNumber(bsonName, value);
        }
    } tcmallocServerStatusSection;
}
}

#ifdef _WIN32
    // gperftools/windows/patch_functions.cc normally defines this function,
    // but we do not link this file since it would dynamically patch our functions.
    // We override the behavior of this function to no-patch functions, but instead
    // simply to do nothing
    // TCMalloc calls this via a static initializer
    void PatchWindowsFunctions() {
        // Intentionally left empty
    }
#endif
