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


#include "mongo/base/string_data_comparator.h"
#ifdef _WIN32
#define NVALGRIND
#endif

#include <cstddef>
#include <memory>
#include <utility>

#include <valgrind/valgrind.h>

#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/commands/server_status.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/tenant_id.h"
#include "mongo/util/tcmalloc_parameters_gen.h"

#ifdef MONGO_HAVE_GOOGLE_TCMALLOC
#include <tcmalloc/malloc_extension.h>
auto static tcmallocProperties = tcmalloc::MallocExtension::GetProperties();
#elif defined(MONGO_HAVE_GPERF_TCMALLOC)
#include <gperftools/malloc_extension.h>
auto static mallocExtensionAPI = MallocExtension::instance();
#endif

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

        auto getValueIfExists = [&](StringData property) -> boost::optional<size_t> {
#ifdef MONGO_HAVE_GOOGLE_TCMALLOC
            if (auto value = tcmallocProperties.find(property.toString());
                value != tcmallocProperties.end()) {
                return {value->second.value};
            }
#elif defined(MONGO_HAVE_GPERF_TCMALLOC)
            size_t value;
            if (mallocExtensionAPI->GetNumericProperty(property.rawData(), &value)) {
                return {value};
            }
#endif
            return boost::none;
        };

        auto tryAppend = [&](BSONObjBuilder& builder, StringData bsonName, StringData property) {
            if (auto value = getValueIfExists(property); !!value) {
                builder.appendNumber(bsonName, static_cast<long long>(*value));
            }
        };

        auto tryStat = [&](BSONObjBuilder& builder, StringData topic, StringData base) {
            tryAppend(builder, base, fmt::format("{}.{}", topic, base));
        };

        // For a list of properties see the "Generic Tcmalloc Status" section of
        // http://google-perftools.googlecode.com/svn/trunk/doc/tcmalloc.html and
        // http://code.google.com/p/gperftools/source/browse/src/gperftools/malloc_extension.h
        {
            BSONObjBuilder sub(builder.subobjStart("generic"));
            tryStat(sub, "generic", "current_allocated_bytes");
            tryStat(sub, "generic", "heap_size");
        }
        {
            BSONObjBuilder sub(builder.subobjStart("tcmalloc"));
            auto tryTc = [&](StringData key) {
                tryStat(sub, "tcmalloc", key);
            };

            tryTc("pageheap_free_bytes");
            tryTc("pageheap_unmapped_bytes");
            tryTc("max_total_thread_cache_bytes");
            tryTc("current_total_thread_cache_bytes");

            {
                long long total = 0;
                if (auto central = getValueIfExists("tcmalloc.central_cache_free"); !!central) {
                    sub.appendNumber("central_cache_free_bytes", static_cast<long long>(*central));
                    total += *central;
                }
                if (auto transfer = getValueIfExists("tcmalloc.transfer_cache_free"); !!transfer) {
                    sub.appendNumber("transfer_cache_free_bytes",
                                     static_cast<long long>(*transfer));
                    total += *transfer;
                }
                if (auto thread = getValueIfExists("tcmalloc.thread_cache_free"); !!thread) {
                    sub.appendNumber("thread_cache_free_bytes", static_cast<long long>(*thread));
                    total += *thread;
                }
                if (auto cpu = getValueIfExists("tcmalloc.cpu_free"); !!cpu) {
                    sub.appendNumber("cpu_cache_free_bytes", static_cast<long long>(*cpu));
                    total += *cpu;
                }
                sub.appendNumber("total_free_bytes", total);
            }

            tryTc("aggressive_memory_decommit");

            tryTc("pageheap_committed_bytes");
            tryTc("pageheap_scavenge_count");
            tryTc("pageheap_commit_count");
            tryTc("pageheap_total_commit_bytes");
            tryTc("pageheap_decommit_count");
            tryTc("pageheap_total_decommit_bytes");
            tryTc("pageheap_reserve_count");
            tryTc("pageheap_total_reserve_bytes");
            tryTc("spinlock_total_delay_ns");

#ifdef MONGO_HAVE_GOOGLE_TCMALLOC
            sub.appendNumber(
                "release_rate",
                static_cast<long long>(tcmalloc::MallocExtension::GetBackgroundReleaseRate()));
#endif

#if MONGO_HAVE_GPERFTOOLS_SIZE_CLASS_STATS
            if (verbosity >= 2) {
                // Size class information
                std::pair<BSONArrayBuilder, BSONArrayBuilder> builders(
                    builder.subarrayStart("size_classes"), BSONArrayBuilder());

                // Size classes and page heap info is dumped in 1 call so that the performance
                // sensitive tcmalloc page heap lock is only taken once
                mallocExtensionAPI->SizeClasses(&builders, appendSizeClassInfo, appendPageHeapInfo);

                builders.first.done();
                builder.append("page_heap", builders.second.arr());
            }
#endif
#ifdef MONGO_HAVE_GOOGLE_TCMALLOC
            builder.append("formattedString", tcmalloc::MallocExtension::GetStats());
#elif defined(MONGO_HAVE_GPERF_TCMALLOC)
            char buffer[4096];
            mallocExtensionAPI->GetStats(buffer, sizeof buffer);
            builder.append("formattedString", buffer);
#endif
        }

        return builder.obj();
    }

private:
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
