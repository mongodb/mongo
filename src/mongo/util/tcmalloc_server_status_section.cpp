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


#ifdef _WIN32
#define NVALGRIND
#endif

#include "mongo/platform/basic.h"

#include <gperftools/malloc_extension.h>
#include <valgrind/valgrind.h>

#include "mongo/base/init.h"
#include "mongo/db/commands/server_status.h"
#include "mongo/db/service_context.h"
#include "mongo/transport/service_entry_point.h"
#include "mongo/util/tcmalloc_parameters_gen.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault


namespace mongo {

// TODO: Remove these implementations and the associated IDL definition in 4.3.
void TCMallocEnableMarkThreadTemporarilyIdle::append(OperationContext*,
                                                     BSONObjBuilder*,
                                                     StringData,
                                                     const boost::optional<TenantId>&) {}

Status TCMallocEnableMarkThreadTemporarilyIdle::setFromString(StringData,
                                                              const boost::optional<TenantId>&) {
    return Status(ErrorCodes::BadValue,
                  "tcmallocEnableMarkThreadTemporarilyIdle has been removed. Setting this "
                  "parameter has no effect and it will be removed in a future version of "
                  "MongoDB.");
}

namespace {

class TCMallocServerStatusSection : public ServerStatusSection {
public:
    TCMallocServerStatusSection() : ServerStatusSection("tcmalloc") {}

    bool includeByDefault() const override {
        return true;
    }

    BSONObj generateSection(OperationContext* opCtx,
                            const BSONElement& configElement) const override {
        long long verbosity = 1;
        if (configElement) {
            // Relies on the fact that safeNumberLong turns non-numbers into 0.
            long long configValue = configElement.safeNumberLong();
            if (configValue) {
                verbosity = configValue;
            }
        }

        BSONObjBuilder builder;

        // For a list of properties see the "Generic Tcmalloc Status" section of
        // http://google-perftools.googlecode.com/svn/trunk/doc/tcmalloc.html and
        // http://code.google.com/p/gperftools/source/browse/src/gperftools/malloc_extension.h
        {
            BSONObjBuilder sub(builder.subobjStart("generic"));
            appendNumericPropertyIfAvailable(
                sub, "current_allocated_bytes", "generic.current_allocated_bytes");
            appendNumericPropertyIfAvailable(sub, "heap_size", "generic.heap_size");
        }
        {
            BSONObjBuilder sub(builder.subobjStart("tcmalloc"));

            appendNumericPropertyIfAvailable(
                sub, "pageheap_free_bytes", "tcmalloc.pageheap_free_bytes");
            appendNumericPropertyIfAvailable(
                sub, "pageheap_unmapped_bytes", "tcmalloc.pageheap_unmapped_bytes");
            appendNumericPropertyIfAvailable(
                sub, "max_total_thread_cache_bytes", "tcmalloc.max_total_thread_cache_bytes");
            appendNumericPropertyIfAvailable(sub,
                                             "current_total_thread_cache_bytes",
                                             "tcmalloc.current_total_thread_cache_bytes");
            // Not including tcmalloc.slack_bytes since it is deprecated.

            // Calculate total free bytes, *excluding the page heap*
            size_t central;
            size_t transfer;
            size_t thread;
            if (MallocExtension::instance()->GetNumericProperty("tcmalloc.central_cache_free_bytes",
                                                                &central) &&
                MallocExtension::instance()->GetNumericProperty(
                    "tcmalloc.transfer_cache_free_bytes", &transfer) &&
                MallocExtension::instance()->GetNumericProperty("tcmalloc.thread_cache_free_bytes",
                                                                &thread)) {
                sub.appendNumber("total_free_bytes",
                                 static_cast<long long>(central + transfer + thread));
            }
            appendNumericPropertyIfAvailable(
                sub, "central_cache_free_bytes", "tcmalloc.central_cache_free_bytes");
            appendNumericPropertyIfAvailable(
                sub, "transfer_cache_free_bytes", "tcmalloc.transfer_cache_free_bytes");
            appendNumericPropertyIfAvailable(
                sub, "thread_cache_free_bytes", "tcmalloc.thread_cache_free_bytes");
            appendNumericPropertyIfAvailable(
                sub, "aggressive_memory_decommit", "tcmalloc.aggressive_memory_decommit");

            appendNumericPropertyIfAvailable(
                sub, "pageheap_committed_bytes", "tcmalloc.pageheap_committed_bytes");
            appendNumericPropertyIfAvailable(
                sub, "pageheap_scavenge_count", "tcmalloc.pageheap_scavenge_count");
            appendNumericPropertyIfAvailable(
                sub, "pageheap_commit_count", "tcmalloc.pageheap_commit_count");
            appendNumericPropertyIfAvailable(
                sub, "pageheap_total_commit_bytes", "tcmalloc.pageheap_total_commit_bytes");
            appendNumericPropertyIfAvailable(
                sub, "pageheap_decommit_count", "tcmalloc.pageheap_decommit_count");
            appendNumericPropertyIfAvailable(
                sub, "pageheap_total_decommit_bytes", "tcmalloc.pageheap_total_decommit_bytes");
            appendNumericPropertyIfAvailable(
                sub, "pageheap_reserve_count", "tcmalloc.pageheap_reserve_count");
            appendNumericPropertyIfAvailable(
                sub, "pageheap_total_reserve_bytes", "tcmalloc.pageheap_total_reserve_bytes");
            appendNumericPropertyIfAvailable(
                sub, "spinlock_total_delay_ns", "tcmalloc.spinlock_total_delay_ns");

            auto tcmallocReleaseRate = MallocExtension::instance()->GetMemoryReleaseRate();
            sub.appendNumber("release_rate", tcmallocReleaseRate);

#if MONGO_HAVE_GPERFTOOLS_SIZE_CLASS_STATS
            if (verbosity >= 2) {
                // Size class information
                std::pair<BSONArrayBuilder, BSONArrayBuilder> builders(
                    builder.subarrayStart("size_classes"), BSONArrayBuilder());

                // Size classes and page heap info is dumped in 1 call so that the performance
                // sensitive tcmalloc page heap lock is only taken once
                MallocExtension::instance()->SizeClasses(
                    &builders, appendSizeClassInfo, appendPageHeapInfo);

                builders.first.done();
                builder.append("page_heap", builders.second.arr());
            }
#endif

            char buffer[4096];
            MallocExtension::instance()->GetStats(buffer, sizeof buffer);
            builder.append("formattedString", buffer);
        }

        return builder.obj();
    }

private:
    static void appendNumericPropertyIfAvailable(BSONObjBuilder& builder,
                                                 StringData bsonName,
                                                 const char* property) {
        size_t value;
        if (MallocExtension::instance()->GetNumericProperty(property, &value))
            builder.appendNumber(bsonName, static_cast<long long>(value));
    }

#if MONGO_HAVE_GPERFTOOLS_SIZE_CLASS_STATS
    static void appendSizeClassInfo(void* bsonarr_builder, const base::MallocSizeClass* stats) {
        BSONArrayBuilder& builder =
            reinterpret_cast<std::pair<BSONArrayBuilder, BSONArrayBuilder>*>(bsonarr_builder)
                ->first;
        BSONObjBuilder doc;

        doc.appendNumber("bytes_per_object", static_cast<long long>(stats->bytes_per_obj));
        doc.appendNumber("pages_per_span", static_cast<long long>(stats->pages_per_span));
        doc.appendNumber("num_spans", static_cast<long long>(stats->num_spans));
        doc.appendNumber("num_thread_objs", static_cast<long long>(stats->num_thread_objs));
        doc.appendNumber("num_central_objs", static_cast<long long>(stats->num_central_objs));
        doc.appendNumber("num_transfer_objs", static_cast<long long>(stats->num_transfer_objs));
        doc.appendNumber("free_bytes", static_cast<long long>(stats->free_bytes));
        doc.appendNumber("allocated_bytes", static_cast<long long>(stats->alloc_bytes));

        builder.append(doc.obj());
    }

    static void appendPageHeapInfo(void* bsonarr_builder, const base::PageHeapSizeClass* stats) {
        BSONArrayBuilder& builder =
            reinterpret_cast<std::pair<BSONArrayBuilder, BSONArrayBuilder>*>(bsonarr_builder)
                ->second;
        BSONObjBuilder doc;

        doc.appendNumber("pages", static_cast<long long>(stats->pages));
        doc.appendNumber("normal_spans", static_cast<long long>(stats->normal_spans));
        doc.appendNumber("unmapped_spans", static_cast<long long>(stats->unmapped_spans));
        doc.appendNumber("normal_bytes", static_cast<long long>(stats->normal_bytes));
        doc.appendNumber("unmapped_bytes", static_cast<long long>(stats->unmapped_bytes));

        builder.append(doc.obj());
    }
#endif
} tcmallocServerStatusSection;
}  // namespace
}  // namespace mongo
