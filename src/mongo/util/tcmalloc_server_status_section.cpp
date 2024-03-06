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


#include <boost/optional/optional.hpp>
#include <cstddef>
#include <optional>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/config.h"
#include "mongo/db/commands/server_status.h"
#include "mongo/db/operation_context.h"
#include "mongo/util/itoa.h"

#ifdef MONGO_CONFIG_TCMALLOC_GOOGLE
#include <tcmalloc/cpu_cache.h>
#include <tcmalloc/malloc_extension.h>
#include <tcmalloc/static_vars.h>
#elif defined(MONGO_CONFIG_TCMALLOC_GPERF)
#include <gperftools/malloc_extension.h>
#endif

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo {
namespace {

template <typename E>
auto getUnderlyingType(E e) {
    return static_cast<std::underlying_type_t<E>>(e);
}

/**
 * For more information about tcmalloc stats, see:
 * https://github.com/google/tcmalloc/blob/master/docs/stats.md and
 * https://github.com/google/tcmalloc/blob/master/tcmalloc/malloc_extension.h
 * for the tcmalloc-google, and
 * https://github.com/gperftools/gperftools/blob/master/docs/tcmalloc.html and
 * https://github.com/gperftools/gperftools/blob/master/src/gperftools/malloc_extension.h
 * for tcmalloc-gperf
 */
class TCMallocMetrics {
public:
    virtual std::vector<StringData> getGenericStatNames() const {
        return {};
    }

    virtual std::vector<StringData> getTCMallocStatNames() const {
        return {};
    }

    virtual boost::optional<long long> getNumericProperty(StringData propertyName) const {
        return boost::none;
    }

    virtual void appendPerCPUMetrics(BSONObjBuilder& bob) const {}

    /**
     * The tcmalloc names for some metrics are misleading, and so we rename them in order to more
     * effectively communicate what they represent.
     */
    virtual void appendRenamedMetrics(BSONObjBuilder& bob) const {}

    virtual long long getReleaseRate() const {
        return 0;
    }

    /**
     * tcmalloc metrics have three verbosity settings.
     *
     * Verbosity is set through serverStatus calls as db.serverStatus({tcmalloc: <verbosity>}).
     * We can configure FTDC to use verbosity: 2 using the diagnosticDataCollectionVerboseTCMalloc
     * server parameter.
     *
     * 1 is the default, and nothing will be appended in this function.
     * 2 will append any additional metrics that we have deemed too large to include by default, but
     * are useful for further diagnostics. This is often numerical data that is useful to view over
     * time.
     * 3 will dump the entire formatted stats string that tcmalloc provides. This should never
     * be included in FTDC, as its size will harm retention.
     */
    virtual void appendHighVerbosityMetrics(BSONObjBuilder& bob, int verbosity) const {}

    virtual void appendCustomDerivedMetrics(BSONObjBuilder& bob) const {}
};

#ifdef MONGO_CONFIG_TCMALLOC_GOOGLE
class GoogleTCMallocMetrics : public TCMallocMetrics {
public:
    std::vector<StringData> getGenericStatNames() const override {
        return {
            "bytes_in_use_by_app"_sd,
            "current_allocated_bytes"_sd,
            "heap_size"_sd,
            "peak_memory_usage"_sd,
            "physical_memory_used"_sd,
            "virtual_memory_used"_sd,
        };
    }

    std::vector<StringData> getTCMallocStatNames() const override {
        return {
            "central_cache_free"_sd,
            "cpu_free"_sd,
            "current_total_thread_cache_bytes"_sd,
            "desired_usage_limit_bytes"_sd,
            "hard_usage_limit_bytes"_sd,
            "local_bytes"_sd,
            "max_total_thread_cache_bytes"_sd,
            "metadata_bytes"_sd,
            "page_algorithm"_sd,
            "pageheap_free_bytes"_sd,
            "pageheap_unmapped_bytes"_sd,
            "required_bytes"_sd,
            "sharded_transfer_cache_free"_sd,
            "thread_cache_count"_sd,
            "thread_cache_free"_sd,
            "transfer_cache_free"_sd,
        };
    }

    boost::optional<long long> getNumericProperty(StringData propertyName) const override {
        if (auto res = tcmalloc::MallocExtension::GetNumericProperty(std::string{propertyName});
            res.has_value()) {
            return static_cast<long long>(res.value());
        }

        return boost::none;
    }

    void appendPerCPUMetrics(BSONObjBuilder& bob) const override {
        _perCPUCachesActive =
            _perCPUCachesActive || tcmalloc::MallocExtension::PerCpuCachesActive();
        bob.appendBool("usingPerCPUCaches", _perCPUCachesActive);
        bob.append("maxPerCPUCacheSizeBytes", tcmalloc::MallocExtension::GetMaxPerCpuCacheSize());
    }

    void appendRenamedMetrics(BSONObjBuilder& bob) const override {
        auto tryAppendRename = [&](StringData statName, StringData newName) {
            if (auto val = getNumericProperty(statName); !!val) {
                bob.appendNumber(newName, *val);
            }
        };
        tryAppendRename("generic.realized_fragmentation", "memory_used_to_memory_held_pct_at_peak");
        tryAppendRename("tcmalloc.external_fragmentation_bytes", "total_bytes_held");
        tryAppendRename("tcmalloc.sampled_internal_fragmentation",
                        "estimated_size_class_overhead_bytes");
    }

    long long getReleaseRate() const override {
        return getUnderlyingType(tcmalloc::MallocExtension::GetBackgroundReleaseRate());
    }

    void appendHighVerbosityMetrics(BSONObjBuilder& bob, int verbosity) const override {
        if (verbosity >= 2 && _perCPUCachesActive) {
            BSONObjBuilder sub(bob.subobjStart("cpuCache"));

            size_t num_cpus = absl::base_internal::NumCPUs();
            auto& cache = tcmalloc::tcmalloc_internal::tc_globals.cpu_cache();

            for (size_t i = 0; i < num_cpus; i++) {
                BSONObjBuilder sub2(sub.subobjStart(ItoA(i)));

                sub2.appendNumber("used_bytes", static_cast<long long>(cache.UsedBytes(i)));
                sub2.appendNumber("allocated", static_cast<long long>(cache.Allocated(i)));
                sub2.appendNumber("unallocated", static_cast<long long>(cache.Unallocated(i)));
                sub2.appendNumber("capacity", static_cast<long long>(cache.Capacity(i)));
                sub2.appendNumber("reclaims", static_cast<long long>(cache.GetNumReclaims(i)));

                auto ms = cache.GetTotalCacheMissStats(i);
                sub2.appendNumber("overflows", static_cast<long long>(ms.overflows));
                sub2.appendNumber("underflows", static_cast<long long>(ms.underflows));
            }
        }

        if (verbosity >= 3) {
            bob.append("formattedString", tcmalloc::MallocExtension::GetStats());
        }
    }

    void appendCustomDerivedMetrics(BSONObjBuilder& bob) const override {
        if (auto physicalMemory = getNumericProperty("generic.physical_memory_used");
            !!physicalMemory) {
            if (auto virtualMemory = getNumericProperty("generic.virtual_memory_used");
                !!virtualMemory) {
                long long unmappedBytes = *virtualMemory - *physicalMemory;
                bob.appendNumber("unmapped_bytes", unmappedBytes);
            }
        }
    }

private:
    // Once per-CPU caches are activated, they cannot be deactivated, and so we cache the true value
    // in order to avoid the FTDC thread loading a contested atomic from tcmalloc when it does not
    // need to.
    static inline bool _perCPUCachesActive = false;
};
#elif defined(MONGO_CONFIG_TCMALLOC_GPERF)
class GperfTCMallocMetrics : public TCMallocMetrics {
public:
    std::vector<StringData> getGenericStatNames() const override {
        return {
            "current_allocated_bytes"_sd,
            "heap_size"_sd,
        };
    }

    std::vector<StringData> getTCMallocStatNames() const override {
        return {
            "pageheap_free_bytes"_sd,
            "pageheap_unmapped_bytes"_sd,
            "max_total_thread_cache_bytes"_sd,
            "current_total_thread_cache_bytes"_sd,
            "central_cache_free_bytes"_sd,
            "transfer_cache_free_bytes"_sd,
            "thread_cache_free_bytes"_sd,
            "aggressive_memory_decommit"_sd,
            "pageheap_committed_bytes"_sd,
            "pageheap_scavenge_count"_sd,
            "pageheap_commit_count"_sd,
            "pageheap_total_commit_bytes"_sd,
            "pageheap_decommit_count"_sd,
            "pageheap_total_decommit_bytes"_sd,
            "pageheap_reserve_count"_sd,
            "pageheap_total_reserve_bytes"_sd,
            "spinlock_total_delay_ns"_sd,
        };
    }

    boost::optional<long long> getNumericProperty(StringData propertyName) const override {
        size_t value;
        if (MallocExtension::instance()->GetNumericProperty(propertyName.rawData(), &value)) {
            return static_cast<long long>(value);
        }

        return boost::none;
    }

    long long getReleaseRate() const override {
        return MallocExtension::instance()->GetMemoryReleaseRate();
    }

    void appendHighVerbosityMetrics(BSONObjBuilder& bob, int verbosity) const override {
        if (verbosity >= 2) {
#if MONGO_HAVE_GPERFTOOLS_SIZE_CLASS_STATS
            // Size class information
            std::pair<BSONArrayBuilder, BSONArrayBuilder> builders(
                bob.subarrayStart("size_classes"), BSONArrayBuilder());

            // Size classes and page heap info is dumped in 1 call so that the performance
            // sensitive tcmalloc page heap lock is only taken once
            MallocExtension::instance()->SizeClasses(
                &builders, appendSizeClassInfo, appendPageHeapInfo);

            builders.first.done();
            bob.append("page_heap", builders.second.arr());
#endif  // MONGO_HAVE_GPERFTOOLS_SIZE_CLASS_STATS
        }

        if (verbosity >= 3) {
            char buffer[4096];
            MallocExtension::instance()->GetStats(buffer, sizeof buffer);
            bob.append("formattedString", buffer);
        }
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
#endif  // MONGO_HAVE_GPERFTOOLS_SIZE_CLASS_STATS
};
#endif  // MONGO_CONFIG_TCMALLOC_GPERF

class TCMallocServerStatusSection : public ServerStatusSection {
public:
    TCMallocServerStatusSection() : ServerStatusSection("tcmalloc") {}

    bool includeByDefault() const override {
        return true;
    }

    BSONObj generateSection(OperationContext* opCtx,
                            const BSONElement& configElement) const override {
        int verbosity = 1;
        if (configElement) {
            // Relies on the fact that safeNumberInt turns non-numbers into 0.
            int configValue = configElement.safeNumberInt();
            if (configValue) {
                verbosity = configValue;
            }
        }

        BSONObjBuilder builder;

        auto tryAppend = [&](BSONObjBuilder& builder, StringData bsonName, StringData property) {
            if (auto value = _metrics.getNumericProperty(property); !!value) {
                builder.appendNumber(bsonName, *value);
            }
        };

        auto tryStat = [&](BSONObjBuilder& builder, StringData topic, StringData base) {
            tryAppend(builder, base, format(FMT_STRING("{}.{}"), topic, base));
        };

        _metrics.appendPerCPUMetrics(builder);
        {
            BSONObjBuilder sub(builder.subobjStart("generic"));
            for (auto& stat : _metrics.getGenericStatNames()) {
                tryStat(sub, "generic", stat);
            }
        }

        {
            BSONObjBuilder sub(builder.subobjStart("tcmalloc"));
            for (auto& stat : _metrics.getTCMallocStatNames()) {
                tryStat(sub, "tcmalloc", stat);
            }

            sub.appendNumber("release_rate", _metrics.getReleaseRate());
            _metrics.appendRenamedMetrics(builder);
            _metrics.appendHighVerbosityMetrics(builder, verbosity);
        }

        {
            BSONObjBuilder sub(builder.subobjStart("tcmalloc_derived"));
            _metrics.appendCustomDerivedMetrics(builder);

            static constexpr std::array totalFreeBytesParts{
                "tcmalloc.pageheap_free_bytes"_sd,
                "tcmalloc.central_cache_free"_sd,
                "tcmalloc.transfer_cache_free"_sd,
                "tcmalloc.thread_cache_free"_sd,
                "tcmalloc.cpu_free"_sd,  // Will be 0 for gperf tcmalloc
            };
            long long total = 0;
            for (auto& stat : totalFreeBytesParts) {
                if (auto value = _metrics.getNumericProperty(stat); !!value) {
                    total += *value;
                }
            }
            sub.appendNumber("total_free_bytes", total);
        }

        return builder.obj();
    }

private:
#ifdef MONGO_CONFIG_TCMALLOC_GOOGLE
    using MyMetrics = GoogleTCMallocMetrics;
#elif defined(MONGO_CONFIG_TCMALLOC_GPERF)
    using MyMetrics = GperfTCMallocMetrics;
#else
    using MyMetrics = TCMallocMetrics;
#endif

    MyMetrics _metrics;
};
TCMallocServerStatusSection tcmallocServerStatusSection;
}  // namespace
}  // namespace mongo
