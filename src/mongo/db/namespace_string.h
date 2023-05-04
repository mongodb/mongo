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

#pragma once

#include <algorithm>
#include <boost/optional.hpp>
#include <iosfwd>
#include <mutex>
#include <string>

#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/database_name.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/server_options.h"
#include "mongo/db/tenant_id.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/uuid.h"

namespace mongo {
class NamespaceStringUtil;

class NamespaceString {
public:
    /**
     * The NamespaceString reserved constants are actually this `ConstantProxy`
     * type, which can be `constexpr` and can be used directly in place of
     * `NamespaceString`, except in very rare cases. To work around those, use a
     * `static_cast<const NamespaceString&>`. The first time it's used, a
     * `ConstantProxy` produces a memoized `const NamespaceString*` and retains
     * it for future uses.
     */
    class ConstantProxy {
    public:
        /**
         * `ConstantProxy` objects can be copied, so that they behave more like
         * `NamespaceString`. All copies will point to the same `SharedState`.
         * The `SharedState` is meant to be defined constexpr, but has mutable
         * data members to implement the on-demand memoization of the
         * `NamespaceString`.
         */
        class SharedState {
        public:
            constexpr SharedState(DatabaseName::ConstantProxy dbName, StringData coll)
                : _db{dbName}, _coll{coll} {}

            const NamespaceString& get() const {
                std::call_once(_once, [this] { _nss = new NamespaceString{_db, _coll}; });
                return *_nss;
            }

        private:
            DatabaseName::ConstantProxy _db;
            StringData _coll;
            mutable std::once_flag _once;
            mutable const NamespaceString* _nss = nullptr;
        };

        constexpr explicit ConstantProxy(const SharedState* sharedState)
            : _sharedState{sharedState} {}

        operator const NamespaceString&() const {
            return _get();
        }

        decltype(auto) ns() const {
            return _get().ns();
        }
        decltype(auto) db() const {
            return _get().db();
        }
        decltype(auto) coll() const {
            return _get().coll();
        }
        decltype(auto) tenantId() const {
            return _get().tenantId();
        }
        decltype(auto) dbName() const {
            return _get().dbName();
        }
        decltype(auto) toString() const {
            return _get().toString();
        }
        decltype(auto) toStringForErrorMsg() const {
            return _get().toStringForErrorMsg();
        }

        friend std::ostream& operator<<(std::ostream& stream, const ConstantProxy& nss) {
            return stream << nss.toString();
        }
        friend StringBuilder& operator<<(StringBuilder& builder, const ConstantProxy& nss) {
            return builder << nss.toString();
        }

    private:
        const NamespaceString& _get() const {
            return _sharedState->get();
        }

        const SharedState* _sharedState;
    };

    constexpr static size_t MaxNSCollectionLenFCV42 = 120U;
    constexpr static size_t MaxNsCollectionLen = 255;

    // The maximum namespace length of sharded collections is less than that of unsharded ones since
    // the namespace of the cached chunks metadata, local to each shard, is composed by the
    // namespace of the related sharded collection (i.e., config.cache.chunks.<ns>).
    constexpr static size_t MaxNsShardedCollectionLen = 235;  // 255 - len(ChunkType::ShardNSPrefix)

    // Reserved system namespaces

    // Name for the system views collection
    static constexpr StringData kSystemDotViewsCollectionName = "system.views"_sd;

    // Name for the system.js collection
    static constexpr StringData kSystemDotJavascriptCollectionName = "system.js"_sd;

    // Name of the pre-images collection.
    static constexpr StringData kPreImagesCollectionName = "system.preimages"_sd;

    // Prefix for the collection storing collection statistics.
    static constexpr StringData kStatisticsCollectionPrefix = "system.statistics."_sd;

    // Name for the change stream change collection.
    static constexpr StringData kChangeCollectionName = "system.change_collection"_sd;

    // Name for the profile collection
    static constexpr StringData kSystemDotProfileCollectionName = "system.profile"_sd;

    // Names of privilege document collections
    static constexpr StringData kSystemUsers = "system.users"_sd;
    static constexpr StringData kSystemRoles = "system.roles"_sd;

    // Prefix for orphan collections
    static constexpr StringData kOrphanCollectionPrefix = "orphan."_sd;

    // Prefix for collections that store the local resharding oplog buffer.
    static constexpr StringData kReshardingLocalOplogBufferPrefix =
        "localReshardingOplogBuffer."_sd;

    // Prefix for resharding conflict stash collections.
    static constexpr StringData kReshardingConflictStashPrefix = "localReshardingConflictStash."_sd;

    // Prefix for temporary resharding collection.
    static constexpr StringData kTemporaryReshardingCollectionPrefix = "system.resharding."_sd;

    // Prefix for time-series buckets collection.
    static constexpr StringData kTimeseriesBucketsCollectionPrefix = "system.buckets."_sd;

    // Prefix for global index container collections. These collections belong to the system
    // database.
    static constexpr StringData kGlobalIndexCollectionPrefix = "globalIndex."_sd;


    // Prefix for the temporary collection used by the $out stage.
    static constexpr StringData kOutTmpCollectionPrefix = "tmp.agg_out."_sd;

    // Maintainers Note: The large set of `NamespaceString`-typed static data
    // members of the `NamespaceString` class representing system-reserved
    // collections is now generated from "namespace_string_reserved.def.h".
    // Please make edits there to add or change such constants.

    // The constants are declared as merely `const` but have `constexpr`
    // definitions below. Because the `NamespaceString` class enclosing their
    // type is incomplete, they can't be _declared_ fully constexpr (a constexpr
    // limitation).
#define NSS_CONSTANT(id, db, coll) static const ConstantProxy id;
#include "namespace_string_reserved.def.h"
#undef NSS_CONSTANT

    /**
     * Constructs an empty NamespaceString.
     */
    NamespaceString() = default;

    /**
     * Constructs a NamespaceString for the given database.
     */
    explicit NamespaceString(DatabaseName dbName) : _data(std::move(dbName._data)) {}

    // TODO SERVER-65920 Remove this constructor once all constructor call sites have been updated
    // to pass tenantId explicitly
    explicit NamespaceString(StringData ns, boost::optional<TenantId> tenantId = boost::none)
        : NamespaceString(std::move(tenantId), ns) {}

    // TODO SERVER-65920 Remove this constructor once all constructor call sites have been updated
    // to pass tenantId explicitly
    NamespaceString(StringData db,
                    StringData collectionName,
                    boost::optional<TenantId> tenantId = boost::none)
        : NamespaceString(DatabaseName(std::move(tenantId), db), collectionName) {}

    /**
     * Constructs a NamespaceString from the string 'ns'. Should only be used when reading a
     * namespace from disk. 'ns' is expected to contain a tenantId when running in Serverless mode.
     */
    // TODO SERVER-70013 Move this function into NamespaceStringUtil, and delegate overlapping
    // functionality to DatabaseNameUtil::parseDbNameFromStringExpectTenantIdInMultitenancyMode.
    static NamespaceString parseFromStringExpectTenantIdInMultitenancyMode(StringData ns);

    /**
     * Constructs a NamespaceString in the global config db, "config.<collName>".
     */
    static NamespaceString makeGlobalConfigCollection(StringData collName);

    /**
     * Constructs a NamespaceString in the local db, "local.<collName>".
     */
    static NamespaceString makeLocalCollection(StringData collName);

    /**
     * These functions construct a NamespaceString without checking for presence of TenantId.
     *
     * MUST only be used for tests.
     */
    static NamespaceString createNamespaceString_forTest(StringData ns) {
        return NamespaceString(boost::none, ns);
    }

    static NamespaceString createNamespaceString_forTest(const DatabaseName& dbName) {
        return NamespaceString(dbName);
    }

    static NamespaceString createNamespaceString_forTest(StringData db, StringData coll) {
        return NamespaceString(boost::none, db, coll);
    }

    static NamespaceString createNamespaceString_forTest(const DatabaseName& dbName,
                                                         StringData coll) {
        return NamespaceString(dbName, coll);
    }

    static NamespaceString createNamespaceString_forTest(const boost::optional<TenantId>& tenantId,
                                                         StringData ns) {
        return NamespaceString(tenantId, ns);
    }

    static NamespaceString createNamespaceString_forTest(const boost::optional<TenantId>& tenantId,
                                                         StringData db,
                                                         StringData coll) {
        return NamespaceString(tenantId, db, coll);
    }

    /**
     * These functions construct a NamespaceString without checking for presence of TenantId. These
     * must only be used by auth systems which are not yet tenant aware.
     *
     * TODO SERVER-74896 Remove this function. Any remaining call sites must be changed to use a
     * function on NamespaceStringUtil.
     */
    static NamespaceString createNamespaceStringForAuth(const boost::optional<TenantId>& tenantId,
                                                        StringData db,
                                                        StringData coll) {
        return NamespaceString(tenantId, db, coll);
    }

    static NamespaceString createNamespaceStringForAuth(const boost::optional<TenantId>& tenantId,
                                                        StringData ns) {
        return NamespaceString(tenantId, ns);
    }

    /**
     * Constructs the namespace '<dbName>.$cmd.aggregate', which we use as the namespace for
     * aggregation commands with the format {aggregate: 1}.
     */
    static NamespaceString makeCollectionlessAggregateNSS(const DatabaseName& dbName);

    /**
     * Constructs the change collection namespace for the specified tenant.
     */
    static NamespaceString makeChangeCollectionNSS(const boost::optional<TenantId>& tenantId);

    /**
     * Constructs the pre-images collection namespace for a tenant if the 'tenantId' is specified,
     * otherwise creates a default pre-images collection namespace.
     */
    static NamespaceString makePreImageCollectionNSS(const boost::optional<TenantId>& tenantId);

    /**
     * Constructs a NamespaceString representing a listCollections namespace. The format for this
     * namespace is "<dbName>.$cmd.listCollections".
     */
    static NamespaceString makeListCollectionsNSS(const DatabaseName& dbName);

    /**
     * Constructs a NamespaceString for the specified global index.
     */
    static NamespaceString makeGlobalIndexNSS(const UUID& uuid);

    /**
     * Constructs the cluster parameters NamespaceString for the specified tenant. The format for
     * this namespace is "(<tenantId>_)config.clusterParameters".
     */
    static NamespaceString makeClusterParametersNSS(const boost::optional<TenantId>& tenantId);

    /**
     * Constructs the system.views NamespaceString for the specified DatabaseName.
     */
    static NamespaceString makeSystemDotViewsNamespace(const DatabaseName& dbName);

    /**
     * Constructs the system.profile NamespaceString for the specified DatabaseName.
     */
    static NamespaceString makeSystemDotProfileNamespace(const DatabaseName& dbName);

    /**
     * Constructs a NamespaceString representing a BulkWrite namespace. The format for this
     * namespace is admin.$cmd.bulkWrite".
     */
    static NamespaceString makeBulkWriteNSS(const boost::optional<TenantId>& tenantId);

    /**
     * Constructs the oplog buffer NamespaceString for the given migration id for movePrimary op.
     */
    static NamespaceString makeMovePrimaryOplogBufferNSS(const UUID& migrationId);

    /**
     * Constructs the NamesapceString to store the collections to clone by the movePrimary op.
     */
    static NamespaceString makeMovePrimaryCollectionsToCloneNSS(const UUID& migrationId);

    /**
     * Constructs the NamespaceString prefix for temporary movePrimary recipient collections.
     */
    static NamespaceString makeMovePrimaryTempCollectionsPrefix(const UUID& migrationId);

    /**
     * Constructs the oplog buffer NamespaceString for the given UUID and donor shardId.
     */
    static NamespaceString makeReshardingLocalOplogBufferNSS(const UUID& existingUUID,
                                                             const std::string& donorShardId);

    /**
     * Constructs the conflict stash NamespaceString for the given UUID and donor shardId.
     */
    static NamespaceString makeReshardingLocalConflictStashNSS(const UUID& existingUUID,
                                                               const std::string& donorShardId);

    /**
     * Constructs the tenant-specific admin.system.users NamespaceString for the given tenant,
     * "tenant_admin.system.users".
     */
    static NamespaceString makeTenantUsersCollection(const boost::optional<TenantId>& tenantId);

    /**
     * Constructs the tenant-specific admin.system.roles NamespaceString for the given tenant,
     * "tenant_admin.system.roles".
     */
    static NamespaceString makeTenantRolesCollection(const boost::optional<TenantId>& tenantId);

    /**
     * Constructs the command NamespaceString, "<dbName>.$cmd".
     */
    static NamespaceString makeCommandNamespace(const DatabaseName& dbName);

    /**
     * Constructs a dummy NamespaceString, "<tenantId>.config.dummy.namespace", to be used where a
     * placeholder NamespaceString is needed. It must be acceptable for tenantId to be empty, so we
     * use "config" as the db.
     */
    static NamespaceString makeDummyNamespace(const boost::optional<TenantId>& tenantId);

    /**
     * NOTE: DollarInDbNameBehavior::allow is deprecated.
     *
     * Please use DollarInDbNameBehavior::disallow and check explicitly for any DB names that must
     * contain a $.
     */
    enum class DollarInDbNameBehavior {
        Disallow,
        Allow,  // Deprecated
    };

    boost::optional<TenantId> tenantId() const {
        if (!_hasTenantId()) {
            return boost::none;
        }

        return TenantId{OID::from(&_data[kDataOffset])};
    }

    StringData db() const {
        // TODO SERVER-65456 Remove this function.
        auto offset = _hasTenantId() ? kDataOffset + OID::kOIDSize : kDataOffset;
        return StringData{_data.data() + offset, _dbNameOffsetEnd()};
    }

    DatabaseName dbName() const {
        auto offset = _hasTenantId() ? kDataOffset + OID::kOIDSize : kDataOffset;
        return DatabaseName{_data.substr(0, offset + _dbNameOffsetEnd()),
                            DatabaseName::TrustedInitTag{}};
    }

    StringData coll() const {
        const auto offset =
            kDataOffset + _dbNameOffsetEnd() + 1 + (_hasTenantId() ? OID::kOIDSize : 0);
        if (offset > _data.size()) {
            return {};
        }

        return StringData{_data.data() + offset, _data.size() - offset};
    }

    StringData ns() const {
        auto offset = _hasTenantId() ? kDataOffset + OID::kOIDSize : kDataOffset;
        return StringData{_data.data() + offset, _data.size() - offset};
    }

    std::string toString() const {
        return ns().toString();
    }

    /**
     * Gets a namespace string with tenant id.
     *
     * MUST only be used for tests.
     */
    std::string toStringWithTenantId_forTest() const {
        return toStringWithTenantId();
    }

    /**
     * This function should only be used when creating a resouce id for nss.
     */
    std::string toStringForResourceId() const {
        return toStringWithTenantId();
    }

    /**
     * This function should only be used when logging a NamespaceString in an error message.
     */
    std::string toStringForErrorMsg() const {
        return toStringWithTenantId();
    }

    /**
     * Method to be used only when logging a NamespaceString in a log message.
     * It is called anytime a NamespaceString is logged by logAttrs or otherwise.
     */
    friend std::string toStringForLogging(const NamespaceString& nss) {
        return nss.toStringWithTenantId();
    }

    size_t size() const {
        auto offset = _hasTenantId() ? kDataOffset + OID::kOIDSize : kDataOffset;
        return _data.size() - offset;
    }

    bool isEmpty() const {
        return _data.size() == kDataOffset;
    }

    //
    // The following methods assume isValid() is true for this NamespaceString.
    //

    bool isHealthlog() const {
        return isLocalDB() && coll() == "system.healthlog";
    }
    bool isSystem() const {
        return coll().startsWith("system.");
    }
    bool isNormalCollection() const {
        return !isSystem() && !(isLocalDB() && coll().startsWith("replset."));
    }
    bool isGlobalIndex() const {
        return coll().startsWith(kGlobalIndexCollectionPrefix);
    }
    bool isAdminDB() const {
        return db() == DatabaseName::kAdmin.db();
    }
    bool isLocalDB() const {
        return db() == DatabaseName::kLocal.db();
    }
    bool isSystemDotProfile() const {
        return coll() == kSystemDotProfileCollectionName;
    }
    bool isSystemDotViews() const {
        return coll() == kSystemDotViewsCollectionName;
    }
    static bool resolvesToSystemDotViews(StringData ns) {
        auto nss = NamespaceString(boost::none, ns);
        return nss.isSystemDotViews();
    }
    bool isSystemDotJavascript() const {
        return coll() == kSystemDotJavascriptCollectionName;
    }
    bool isServerConfigurationCollection() const {
        return isAdminDB() && (coll() == "system.version");
    }
    bool isPrivilegeCollection() const {
        if (!isAdminDB()) {
            return false;
        }
        return (coll() == kSystemUsers) || (coll() == kSystemRoles);
    }
    bool isConfigDB() const {
        return db() == DatabaseName::kConfig.db();
    }
    bool isCommand() const {
        return coll() == "$cmd";
    }
    bool isOplog() const {
        return oplog(ns());
    }
    bool isOnInternalDb() const {
        return isAdminDB() || isLocalDB() || isConfigDB();
    }

    bool isOrphanCollection() const {
        return isLocalDB() && coll().startsWith(kOrphanCollectionPrefix);
    }

    /**
     * Returns whether the specified namespace is used for internal purposes only and can
     * never be marked as anything other than UNSHARDED.
     */
    bool isNamespaceAlwaysUnsharded() const;

    /**
     * Returns whether the specified namespace is config.cache.chunks.<>.
     */
    bool isConfigDotCacheDotChunks() const;

    /**
     * Returns whether the specified namespace is config.localReshardingOplogBuffer.<>.
     */
    bool isReshardingLocalOplogBufferCollection() const;

    /**
     * Returns whether the specified namespace is config.localReshardingConflictStash.<>.
     */
    bool isReshardingConflictStashCollection() const;

    /**
     * Returns whether the specified namespace is <database>.system.resharding.<>.
     */
    bool isTemporaryReshardingCollection() const;

    /**
     * Returns whether the specified namespace is <database>.system.buckets.<>.
     */
    bool isTimeseriesBucketsCollection() const;

    /**
     * Returns whether the specified namespace is config.system.preimages.
     */
    bool isChangeStreamPreImagesCollection() const;

    /**
     * Returns whether the specified namespace is config.system.changeCollection.
     */
    bool isChangeCollection() const;

    /**
     * Returns whether the specified namespace is config.image_collection.
     */
    bool isConfigImagesCollection() const;

    /**
     * Returns whether the specified namespace is config.transactions.
     */
    bool isConfigTransactionsCollection() const;

    /**
     * Returns whether the specified namespace is <database>.enxcol_.<.+>.(esc|ecc|ecoc).
     */
    bool isFLE2StateCollection() const;

    /**
     * Returns true if the namespace is an oplog or a change collection, false otherwise.
     */
    bool isOplogOrChangeCollection() const;

    /**
     * Returns true if the namespace is a system.statistics collection, false otherwise.
     */
    bool isSystemStatsCollection() const;

    /**
     * Returns true if the collection starts with "system.buckets.tmp.agg_out". Used for $out to
     * time-series collections.
     */
    bool isOutTmpBucketsCollection() const;

    /**
     * Returns the time-series buckets namespace for this view.
     */
    NamespaceString makeTimeseriesBucketsNamespace() const;

    /**
     * Returns the time-series view namespace for this buckets namespace.
     */
    NamespaceString getTimeseriesViewNamespace() const;

    /**
     * Returns whether the namespace is implicitly replicated, based only on its string value.
     *
     * An implicitly replicated namespace is an internal namespace which does not replicate writes
     * via the oplog, with the exception of deletions. Writes are not replicated as an optimization
     * because their content can be reliably derived from entries in the oplog.
     */
    bool isImplicitlyReplicated() const;

    /**
     * Returns whether a namespace is replicated, based only on its string value. One notable
     * omission is that map reduce `tmp.mr` collections may or may not be replicated. Callers must
     * decide how to handle that case separately.
     *
     * Note: This function considers "replicated" to be any namespace that should be timestamped.
     * Not all collections that are timestamped are replicated explicitly through the oplog.
     * Drop-pending collections are a notable example. Please use
     * ReplicationCoordinator::isOplogDisabledForNS to determine if a namespace gets logged in the
     * oplog.
     */
    bool isReplicated() const;

    /**
     * The namespace associated with some ClientCursors does not correspond to a particular
     * namespace. For example, this is true for listCollections cursors and $currentOp agg cursors.
     * Returns true if the namespace string is for a "collectionless" cursor.
     */
    bool isCollectionlessCursorNamespace() const {
        return coll().startsWith("$cmd."_sd);
    }

    /**
     * NOTE an aggregate could still refer to another collection using a stage like $out.
     */
    bool isCollectionlessAggregateNS() const;
    bool isListCollectionsCursorNS() const;

    /**
     * Returns true if a client can modify this namespace even though it is under ".system."
     * For example <dbname>.system.users is ok for regular clients to update.
     */
    bool isLegalClientSystemNS(const ServerGlobalParams::FeatureCompatibility& currentFCV) const;

    /**
     * Returns true if this namespace refers to a drop-pending collection.
     */
    bool isDropPendingNamespace() const;

    /**
     * Returns true if operations on this namespace must be applied in their own oplog batch.
     */
    bool mustBeAppliedInOwnOplogBatch() const;

    /**
     * Returns the drop-pending namespace name for this namespace, provided the given optime.
     *
     * Example:
     *     test.foo -> test.system.drop.<timestamp seconds>i<timestamp increment>t<term>.foo
     */
    NamespaceString makeDropPendingNamespace(const repl::OpTime& opTime) const;

    /**
     * Returns the optime used to generate the drop-pending namespace.
     * Returns an error if this namespace is not drop-pending.
     */
    StatusWith<repl::OpTime> getDropPendingNamespaceOpTime() const;

    /**
     * Returns true if the namespace is valid. Special namespaces for internal use are considered as
     * valid.
     */
    bool isValid(DollarInDbNameBehavior behavior = DollarInDbNameBehavior::Allow) const {
        return validDBName(db(), behavior) && !coll().empty();
    }

    /**
     * NamespaceString("foo.bar").getSisterNS("blah") returns "foo.blah".
     */
    std::string getSisterNS(StringData local) const;

    NamespaceString getCommandNS() const {
        return {dbName(), "$cmd"};
    }

    void serializeCollectionName(BSONObjBuilder* builder, StringData fieldName) const;

    /**
     * @return true if the ns is an oplog one, otherwise false.
     */
    static bool oplog(StringData ns) {
        return ns.startsWith("local.oplog.");
    }

    /**
     * samples:
     *   good
     *      foo
     *      bar
     *      foo-bar
     *   bad:
     *      foo bar
     *      foo.bar
     *      foo"bar
     *
     * @param db - a possible database name
     * @param DollarInDbNameBehavior - please do not change the default value. DB names that must
     *                                 contain a $ should be checked explicitly.
     * @return if db is an allowed database name
     */
    static bool validDBName(StringData dbName,
                            DollarInDbNameBehavior behavior = DollarInDbNameBehavior::Disallow);

    static bool validDBName(const DatabaseName& dbName,
                            DollarInDbNameBehavior behavior = DollarInDbNameBehavior::Disallow) {
        return validDBName(dbName.db(), behavior);
    }

    /**
     * Takes a fully qualified namespace (ie dbname.collectionName), and returns true if
     * the collection name component of the namespace is valid.
     * samples:
     *   good:
     *      foo.bar
     *   bad:
     *      foo.
     *
     * @param ns - a full namespace (a.b)
     * @return if db.coll is an allowed collection name
     */
    static bool validCollectionComponent(StringData ns);

    /**
     * Takes a collection name and returns true if it is a valid collection name.
     * samples:
     *   good:
     *     foo
     *     system.views
     *   bad:
     *     $foo
     * @param coll - a collection name component of a namespace
     * @return if the input is a valid collection name
     */
    static bool validCollectionName(StringData coll);

    friend std::ostream& operator<<(std::ostream& stream, const NamespaceString& nss) {
        return stream << nss.toString();
    }

    friend StringBuilder& operator<<(StringBuilder& builder, const NamespaceString& nss) {
        return builder << nss.toString();
    }

    int compare(const NamespaceString& other) const {
        if (_hasTenantId() && !other._hasTenantId()) {
            return 1;
        }

        if (other._hasTenantId() && !_hasTenantId()) {
            return -1;
        }

        return StringData{_data.data() + kDataOffset, _data.size() - kDataOffset}.compare(
            StringData{other._data.data() + kDataOffset, other._data.size() - kDataOffset});
    }

    friend bool operator==(const NamespaceString& lhs, const NamespaceString& rhs) {
        return lhs._data == rhs._data;
    }

    friend bool operator!=(const NamespaceString& lhs, const NamespaceString& rhs) {
        return lhs._data != rhs._data;
    }

    friend bool operator<(const NamespaceString& lhs, const NamespaceString& rhs) {
        return lhs.compare(rhs) < 0;
    }

    friend bool operator<=(const NamespaceString& lhs, const NamespaceString& rhs) {
        return lhs.compare(rhs) <= 0;
    }

    friend bool operator>(const NamespaceString& lhs, const NamespaceString& rhs) {
        return lhs.compare(rhs) > 0;
    }

    friend bool operator>=(const NamespaceString& lhs, const NamespaceString& rhs) {
        return lhs.compare(rhs) >= 0;
    }

    template <typename H>
    friend H AbslHashValue(H h, const NamespaceString& nss) {
        return H::combine(std::move(h), nss._data);
    }

    friend auto logAttrs(const NamespaceString& nss) {
        return "namespace"_attr = nss;
    }

private:
    friend NamespaceStringUtil;

    /**
     * In order to construct NamespaceString objects, use NamespaceStringUtil. The functions
     * on NamespaceStringUtil make assertions necessary when running in Serverless.
     */

    /**
     * Constructs a NamespaceString from the fully qualified namespace named in "ns" and the
     * tenantId. "ns" is NOT expected to contain the tenantId.
     */
    explicit NamespaceString(boost::optional<TenantId> tenantId, StringData ns) {
        uassert(ErrorCodes::InvalidNamespace,
                "namespaces cannot have embedded null characters",
                ns.find('\0') == std::string::npos);

        auto dotIndex = ns.find('.');
        uassert(ErrorCodes::InvalidNamespace,
                fmt::format("db name must be at most {} characters, found: {}",
                            DatabaseName::kMaxDatabaseNameLength,
                            dotIndex),
                (dotIndex != std::string::npos ? dotIndex : ns.size()) <=
                    DatabaseName::kMaxDatabaseNameLength);

        uint8_t details =
            (dotIndex != std::string::npos ? dotIndex : ns.size()) & kDatabaseNameOffsetEndMask;
        size_t dbStartIndex = kDataOffset;
        if (tenantId) {
            dbStartIndex += OID::kOIDSize;
            details |= kTenantIdMask;
        }

        _data.resize(dbStartIndex + ns.size());
        *reinterpret_cast<uint8_t*>(_data.data()) = details;
        if (tenantId) {
            std::memcpy(_data.data() + kDataOffset, tenantId->_oid.view().view(), OID::kOIDSize);
        }
        if (!ns.empty()) {
            std::memcpy(_data.data() + dbStartIndex, ns.rawData(), ns.size());
        }
    }

    /**
     * Constructs a NamespaceString for the given database and collection names.
     * "dbName" must not contain a ".", and "collectionName" must not start with one.
     */
    NamespaceString(DatabaseName dbName, StringData collectionName) {
        const auto db = dbName.db();
        uassert(ErrorCodes::InvalidNamespace,
                "Collection names cannot start with '.': " + collectionName,
                collectionName.empty() || collectionName[0] != '.');

        auto ns = (str::stream() << db << '.' << collectionName).ss.str();
        uassert(ErrorCodes::InvalidNamespace,
                "namespaces cannot have embedded null characters",
                ns.find('\0') == std::string::npos);

        _data = dbName._data + "."_sd + collectionName;
    }

    /**
     * Constructs a NamespaceString for the given db name, collection name, and tenantId.
     * "db" must not contain a ".", and "collectionName" must not start with one. "db" is
     * NOT expected to contain a tenantId.
     */
    NamespaceString(boost::optional<TenantId> tenantId, StringData db, StringData collectionName)
        : NamespaceString(DatabaseName(std::move(tenantId), db), collectionName) {}

    std::string toStringWithTenantId() const {
        if (_hasTenantId()) {
            auto tenantId = TenantId{OID::from(&_data[kDataOffset])};
            return str::stream() << tenantId.toString() << "_" << ns();
        }

        return ns().toString();
    }

    static constexpr size_t kDataOffset = sizeof(uint8_t);
    static constexpr uint8_t kTenantIdMask = 0x80;
    static constexpr uint8_t kDatabaseNameOffsetEndMask = 0x7F;

    inline bool _hasTenantId() const {
        return static_cast<uint8_t>(_data.front()) & kTenantIdMask;
    }

    inline size_t _dbNameOffsetEnd() const {
        return static_cast<uint8_t>(_data.front()) & kDatabaseNameOffsetEndMask;
    }

    // In order to reduce the size of a NamespaceString, we pack all possible namespace data
    // into a single std::string with the following in-memory layout:
    //
    //      1 byte         12 byte optional tenant id               remaining bytes
    //    discriminator       (see more below)                        namespace
    //  |<------------->|<--------------------------->|<-------------------------------------->|
    //  [---------------|----|----|----|----|----|----|----|----|----|----|----|----|----|----|]
    //  0               1                            12                                       ??
    //
    // The MSB of the discriminator tells us whether a tenant id is present, and the remaining
    // bits store the offset of end of the databaes component of the namespace. Database names
    // must be 64 characters or shorter, so we can be confident the length will fit in three bits.

    std::string _data{'\0'};
};

/**
 * This class is intented to be used by commands which can accept either a collection name or
 * database + collection UUID. It will never be initialized with both.
 */
class NamespaceStringOrUUID {
public:
    NamespaceStringOrUUID() = delete;
    NamespaceStringOrUUID(NamespaceString nss) : _nss(std::move(nss)) {}
    NamespaceStringOrUUID(const NamespaceString::ConstantProxy& nss)
        : NamespaceStringOrUUID{static_cast<const NamespaceString&>(nss)} {}
    NamespaceStringOrUUID(DatabaseName dbname, UUID uuid)
        : _uuid(std::move(uuid)), _dbname(std::move(dbname)) {}
    NamespaceStringOrUUID(boost::optional<TenantId> tenantId, std::string db, UUID uuid)
        : _uuid(std::move(uuid)), _dbname(DatabaseName(std::move(tenantId), std::move(db))) {}
    // TODO SERVER-65920 Remove once all call sites have been changed to take tenantId explicitly
    NamespaceStringOrUUID(std::string db,
                          UUID uuid,
                          boost::optional<TenantId> tenantId = boost::none)
        : _uuid(std::move(uuid)), _dbname(DatabaseName(std::move(tenantId), std::move(db))) {}

    const boost::optional<NamespaceString>& nss() const {
        return _nss;
    }

    void setNss(const NamespaceString& nss) {
        _nss = nss;
    }

    const boost::optional<UUID>& uuid() const {
        return _uuid;
    }

    /**
     * Returns database name if this object was initialized with a UUID.
     *
     * TODO SERVER-66887 remove this function for better clarity once call sites have been changed
     */
    std::string dbname() const {
        return _dbname ? _dbname->db().toString() : "";
    }

    void preferNssForSerialization() {
        _preferNssForSerialization = true;
    }

    /**
     * Returns database name derived from either '_nss' or '_dbname'.
     */
    DatabaseName dbName() const {
        return _nss ? _nss->dbName() : *_dbname;
    }

    /**
     * Returns OK if either the nss is not set or is a valid nss. Otherwise returns an
     * InvalidNamespace error.
     */
    Status isNssValid() const;

    std::string toString() const;

    std::string toStringForErrorMsg() const;

    void serialize(BSONObjBuilder* builder, StringData fieldName) const;

    friend std::ostream& operator<<(std::ostream& stream, const NamespaceStringOrUUID& o) {
        return stream << o.toString();
    }

    friend StringBuilder& operator<<(StringBuilder& builder, const NamespaceStringOrUUID& o) {
        return builder << o.toString();
    }

private:
    // At any given time exactly one of these optionals will be initialized.
    boost::optional<NamespaceString> _nss;
    boost::optional<UUID> _uuid;

    // When seralizing, if both '_nss' and '_uuid' are present, use '_nss'.
    bool _preferNssForSerialization = false;

    // Empty when '_nss' is non-none, and contains the database name when '_uuid' is
    // non-none. Although the UUID specifies a collection uniquely, we must later verify that the
    // collection belongs to the database named here.
    boost::optional<DatabaseName> _dbname;
};

/**
 * "database.a.b.c" -> "database"
 */
inline StringData nsToDatabaseSubstring(StringData ns) {
    size_t i = ns.find('.');
    if (i == std::string::npos) {
        massert(
            10078, "nsToDatabase: db too long", ns.size() <= DatabaseName::kMaxDatabaseNameLength);
        return ns;
    }
    massert(10088, "nsToDatabase: db too long", i <= DatabaseName::kMaxDatabaseNameLength);
    return ns.substr(0, i);
}

/**
 * "database.a.b.c" -> "database"
 *
 * TODO: make this return a StringData
 */
inline std::string nsToDatabase(StringData ns) {
    return nsToDatabaseSubstring(ns).toString();
}

/**
 * "database.a.b.c" -> "a.b.c"
 */
inline StringData nsToCollectionSubstring(StringData ns) {
    size_t i = ns.find('.');
    massert(16886, "nsToCollectionSubstring: no .", i != std::string::npos);
    return ns.substr(i + 1);
}

/**
 * foo = false
 * foo. = false
 * foo.a = true
 */
inline bool nsIsFull(StringData ns) {
    size_t i = ns.find('.');
    if (i == std::string::npos)
        return false;
    if (i == ns.size() - 1)
        return false;
    return true;
}

/**
 * foo = true
 * foo. = false
 * foo.a = false
 */
inline bool nsIsDbOnly(StringData ns) {
    size_t i = ns.find('.');
    if (i == std::string::npos)
        return true;
    return false;
}

inline bool NamespaceString::validDBName(StringData db, DollarInDbNameBehavior behavior) {
    if (db.size() == 0 || db.size() > DatabaseName::kMaxDatabaseNameLength)
        return false;

    for (StringData::const_iterator iter = db.begin(), end = db.end(); iter != end; ++iter) {
        switch (*iter) {
            case '\0':
            case '/':
            case '\\':
            case '.':
            case ' ':
            case '"':
                return false;
            case '$':
                if (behavior == DollarInDbNameBehavior::Disallow)
                    return false;
                continue;
#ifdef _WIN32
            // We prohibit all FAT32-disallowed characters on Windows
            case '*':
            case '<':
            case '>':
            case ':':
            case '|':
            case '?':
                return false;
#endif
            default:
                continue;
        }
    }
    return true;
}

inline bool NamespaceString::validCollectionComponent(StringData ns) {
    size_t idx = ns.find('.');
    if (idx == std::string::npos)
        return false;

    return validCollectionName(ns.substr(idx + 1)) || oplog(ns);
}

inline bool NamespaceString::validCollectionName(StringData coll) {
    if (coll.empty())
        return false;

    if (coll[0] == '.')
        return false;

    for (StringData::const_iterator iter = coll.begin(), end = coll.end(); iter != end; ++iter) {
        switch (*iter) {
            case '\0':
            case '$':
                return false;
            default:
                continue;
        }
    }

    return true;
}

// Here are the `constexpr` definitions for the `NamespaceString::ConstantProxy`
// constant static data members of `NamespaceString`. They cannot be defined
// `constexpr` inside the class definition, but they can be upgraded to
// `constexpr` here below it. Each one needs to be initialized with the address
// of their associated shared state, so those are all defined first, as
// variables named by the same `id`, but in separate nested namespace.
namespace nss_detail::const_proxy_shared_states {
#define NSS_CONSTANT(id, db, coll) \
    constexpr inline NamespaceString::ConstantProxy::SharedState id{db, coll};
#include "namespace_string_reserved.def.h"
#undef NSS_CONSTANT
}  // namespace nss_detail::const_proxy_shared_states

#define NSS_CONSTANT(id, db, coll)                                       \
    constexpr inline NamespaceString::ConstantProxy NamespaceString::id{ \
        &nss_detail::const_proxy_shared_states::id};
#include "namespace_string_reserved.def.h"
#undef NSS_CONSTANT

}  // namespace mongo
