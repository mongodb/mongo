// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/read_write_concern_defaults_gen.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/service_context.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/platform/atomic.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/concurrency/thread_pool_interface.h"
#include "mongo/util/functional.h"
#include "mongo/util/modules.h"
#include "mongo/util/read_through_cache.h"
#include "mongo/util/time_support.h"

#include <mutex>
#include <string_view>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace [[MONGO_MOD_PUBLIC]] mongo {
using namespace std::literals::string_view_literals;

/**
 * Class to manage Read Concern and Write Concern (RWC) defaults.
 */
class ReadWriteConcernDefaults {
public:
    /**
     * Consistent names for the classes that define a ReadConcern and a WriteConcern.
     */
    using ReadConcern = repl::ReadConcernArgs;
    using WriteConcern = WriteConcernOptions;

    using FetchDefaultsFn = unique_function<boost::optional<RWConcernDefault>(OperationContext*)>;

    static constexpr std::string_view readConcernFieldName = ReadConcern::kReadConcernFieldName;
    static constexpr std::string_view writeConcernFieldName = WriteConcern::kWriteConcernField;

    // The _id of the persisted default read/write concern document.
    static constexpr std::string_view kPersistedDocumentId = "ReadWriteConcernDefaults"sv;

    static boost::optional<ReadWriteConcernDefaults>& getDecoration(Service* service);

    static ReadWriteConcernDefaults& get(Service* service) {
        return *getDecoration(service);
    }

    static ReadWriteConcernDefaults& get(OperationContext* opCtx) {
        return *getDecoration(opCtx->getService());
    }

    static void create(Service* service, FetchDefaultsFn fetchDefaultsFn);

    ReadWriteConcernDefaults(Service* service, FetchDefaultsFn fetchDefaultsFn);
    ~ReadWriteConcernDefaults();

    /**
     * Shut down and join thread pool. Needs to be called before destruction.
     */
    void shutDownAndJoin();

    /**
     * Syntactic sugar around 'getDefault' below. A return value of boost::none means that there is
     * no default specified for that particular concern.
     */
    boost::optional<ReadConcern> getDefaultReadConcern(OperationContext* opCtx);
    boost::optional<WriteConcern> getDefaultWriteConcern(OperationContext* opCtx);

    /**
     * Returns the implicit default read concern.
     */
    repl::ReadConcernArgs getImplicitDefaultReadConcern();

    class RWConcernDefaultAndTime : public RWConcernDefault {
    public:
        RWConcernDefaultAndTime() = default;
        RWConcernDefaultAndTime(RWConcernDefault rwcd, Date_t localUpdateWallClockTime)
            : RWConcernDefault(std::move(rwcd)),
              _localUpdateWallClockTime(localUpdateWallClockTime) {}

        Date_t localUpdateWallClockTime() const {
            return _localUpdateWallClockTime;
        }

    private:
        Date_t _localUpdateWallClockTime;
    };

    /**
     * Returns the current set of read/write concern defaults along with the wallclock time when
     * they were cached (for diagnostic purposes).
     */
    RWConcernDefaultAndTime getDefault(OperationContext* opCtx);

    /**
     * Returns true if the RC level is permissible to use as a default, and false if it cannot be a
     * RC default.
     */
    static bool isSuitableReadConcernLevel(repl::ReadConcernLevel level);

    /**
     * Checks if the given RWC is suitable to use as a default, and uasserts if not.
     */
    static void checkSuitabilityAsDefault(const ReadConcern& rc);
    static void checkSuitabilityAsDefault(const WriteConcern& wc,
                                          bool writeConcernMajorityShouldJournal);

    /**
     * Examines a document key affected by a write to config.settings and will register a WUOW
     * onCommit handler that invalidates this cache when the operation commits if the write affects
     * the read/write concern defaults document.
     */
    void observeDirectWriteToConfigSettings(OperationContext* opCtx,
                                            BSONElement idElem,
                                            boost::optional<BSONObj> newDoc);

    /**
     * Generates a new read and write concern default to be persisted on disk, without updating the
     * cached value.
     * At least one of the `rc` or `wc` params must be set.
     * Will generate and use a new epoch and setTime for the updated defaults, which are returned.
     * Validates the supplied read and write concerns can serve as defaults.
     */
    RWConcernDefault generateNewCWRWCToBeSavedOnDisk(OperationContext* opCtx,
                                                     const boost::optional<ReadConcern>& rc,
                                                     const boost::optional<WriteConcern>& wc);

    /**
     * Returns true if cluster-wide write concern is set.
     */
    bool isCWWCSet(OperationContext* opCtx);

    /**
     * Returns true if a cluster-wide read concern default may have been set.
     * This is a cheap atomic check suitable for fast-path decisions.
     * It may briefly return true after a CWRC is unset (until the cache refreshes),
     * but will never return false when a CWRC is actually set.
     */
    bool isCWRCSetFast() const {
        return _customDefaultReadConcernSet.loadRelaxed();
    }

    /**
     * Invalidates the cached RWC defaults, causing them to be refreshed.
     *
     * After this call returns, the read methods below (getDefault, getDefaultReadConcern,
     * getDefaultWriteConcern) may continue returning the invalidated defaults, until they have been
     * replaced by the refreshed values.  This is to avoid stalling CRUD ops (and other ops that
     * need RC/WC) during refresh.
     */
    void invalidate();

    /**
     * Manually looks up the latest defaults, and if their epoch is more recent than the cached
     * defaults or indicates there are no defaults, then update the cache with the new defaults.
     */
    void refreshIfNecessary(OperationContext* opCtx);

    /**
     * Sets the given read write concern as the defaults in the cache.
     */
    void setDefault(OperationContext* opCtx, RWConcernDefault&& rwc);

    /**
     * Sets implicit default write concern whether it should be majority or not.
     * Should be called only once on startup (except in testing).
     */
    void setImplicitDefaultWriteConcernMajority(bool newImplicitDefaultWCMajority);

    /**
     * Gets a bool indicating whether the implicit default write concern is majority.
     * This function should only be used for testing purposes.
     */
    [[MONGO_MOD_PARENT_PRIVATE]] bool getImplicitDefaultWriteConcernMajority_forTest();

    /**
     * Gets the cluster-wide write concern (CWWC) persisted on disk.
     */
    boost::optional<WriteConcern> getCWWC(OperationContext* opCtx);

private:
    enum class Type { kReadWriteConcernEntry };

    /**
     * Gets cluster wide read and write concerns (CWRWC) persisted on disk.
     */
    boost::optional<RWConcernDefaultAndTime> _getDefaultCWRWCFromDisk(OperationContext* opCtx);

    class Cache : public ReadThroughCache<Type, RWConcernDefault> {
        Cache(const Cache&) = delete;
        Cache& operator=(const Cache&) = delete;

    public:
        Cache(Service* service, ThreadPoolInterface& threadPool, FetchDefaultsFn fetchDefaultsFn);
        ~Cache() override = default;

        boost::optional<RWConcernDefault> lookup(OperationContext* opCtx);

    private:
        std::mutex _mutex;

        FetchDefaultsFn _fetchDefaultsFn;
    };

    // Thread pool on which to perform loading of the cached RWC defaults.
    // Must be declared before '_defaults' so it is initialized first and destroyed last,
    // since '_defaults' holds a reference to '_threadPool'.
    ThreadPool _threadPool;

    Cache _defaults;

    // Indicate whether implicit default write concern should be majority or not.
    Atomic<bool> _implicitDefaultWriteConcernMajority;

    Atomic<bool> _customDefaultReadConcernSet{false};
};

}  // namespace mongo
