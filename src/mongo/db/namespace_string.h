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

class NamespaceString {
public:
    constexpr static size_t MaxDatabaseNameLen =
        128;  // max str len for the db name, including null char
    constexpr static size_t MaxNSCollectionLenFCV42 = 120U;
    constexpr static size_t MaxNsCollectionLen = 255;

    // The maximum namespace length of sharded collections is less than that of unsharded ones since
    // the namespace of the cached chunks metadata, local to each shard, is composed by the
    // namespace of the related sharded collection (i.e., config.cache.chunks.<ns>).
    constexpr static size_t MaxNsShardedCollectionLen = 235;  // 255 - len(ChunkType::ShardNSPrefix)

    // Reserved system namespaces

    // Namespace for the admin database
    static constexpr StringData kAdminDb = "admin"_sd;

    // Namespace for the local database
    static constexpr StringData kLocalDb = "local"_sd;

    // Namespace for the system database
    static constexpr StringData kSystemDb = "system"_sd;

    // Namespace for the sharding config database
    static constexpr StringData kConfigDb = "config"_sd;

    // The $external database used by X.509, LDAP, etc...
    static constexpr StringData kExternalDb = "$external"_sd;

    // Name for the system views collection
    static constexpr StringData kSystemDotViewsCollectionName = "system.views"_sd;

    // Name for the system.js collection
    static constexpr StringData kSystemDotJavascriptCollectionName = "system.js"_sd;

    // Name for the change stream change collection.
    static constexpr StringData kChangeCollectionName = "system.change_collection"_sd;

    // Names of privilege document collections
    static constexpr StringData kSystemUsers = "system.users"_sd;
    static constexpr StringData kSystemRoles = "system.roles"_sd;

    // Prefix for orphan collections
    static constexpr StringData kOrphanCollectionPrefix = "orphan."_sd;
    static constexpr StringData kOrphanCollectionDb = "local"_sd;

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
    static constexpr StringData kGlobalIndexCollectionPrefix = "globalIndexes."_sd;

    // Namespace for storing configuration data, which needs to be replicated if the server is
    // running as a replica set. Documents in this collection should represent some configuration
    // state of the server, which needs to be recovered/consulted at startup. Each document in this
    // namespace should have its _id set to some string, which meaningfully describes what it
    // represents. For example, 'shardIdentity' and 'featureCompatibilityVersion'.
    static const NamespaceString kServerConfigurationNamespace;

    // Namespace for storing the logical sessions information
    static const NamespaceString kLogicalSessionsNamespace;

    // Namespace for storing databases information
    static const NamespaceString kConfigDatabasesNamespace;

    // Namespace for storing the transaction information for each session
    static const NamespaceString kSessionTransactionsTableNamespace;

    // Name for a shard's collections metadata collection, each document of which indicates the
    // state of a specific collection
    static const NamespaceString kShardConfigCollectionsNamespace;

    // Name for a shard's databases metadata collection, each document of which indicates the state
    // of a specific database
    static const NamespaceString kShardConfigDatabasesNamespace;

    // Namespace for storing keys for signing and validating cluster times created by the cluster
    // that this node is in.
    static const NamespaceString kKeysCollectionNamespace;

    // Namespace for storing keys for validating cluster times created by other clusters.
    static const NamespaceString kExternalKeysCollectionNamespace;

    // Namespace of the the oplog collection.
    static const NamespaceString kRsOplogNamespace;

    // Namespace for storing the persisted state of transaction coordinators.
    static const NamespaceString kTransactionCoordinatorsNamespace;

    // Namespace for storing the persisted state of migration coordinators.
    static const NamespaceString kMigrationCoordinatorsNamespace;

    // Namespace for storing the persisted state of migration recipients.
    static const NamespaceString kMigrationRecipientsNamespace;

    // Namespace for storing the persisted state of tenant migration donors.
    static const NamespaceString kTenantMigrationDonorsNamespace;

    // Namespace for storing the persisted state of tenant migration recipient service instances.
    static const NamespaceString kTenantMigrationRecipientsNamespace;

    // Namespace for view on local.oplog.rs for tenant migrations.
    static const NamespaceString kTenantMigrationOplogView;

    // Namespace for storing the persisted state of tenant split donors.
    static const NamespaceString kShardSplitDonorsNamespace;

    // Namespace for replica set configuration settings.
    static const NamespaceString kSystemReplSetNamespace;

    // Namespace for storing the last replica set election vote.
    static const NamespaceString kLastVoteNamespace;

    // Namespace for change stream pre-images collection.
    static const NamespaceString kChangeStreamPreImagesNamespace;

    // Namespace for index build entries.
    static const NamespaceString kIndexBuildEntryNamespace;

    // Namespace for pending range deletions.
    static const NamespaceString kRangeDeletionNamespace;

    // Namespace containing pending range deletions snapshots for rename operations.
    static const NamespaceString kRangeDeletionForRenameNamespace;

    // Namespace for the coordinator's resharding operation state.
    static const NamespaceString kConfigReshardingOperationsNamespace;

    // Namespace for the donor shard's local resharding operation state.
    static const NamespaceString kDonorReshardingOperationsNamespace;

    // Namespace for the recipient shard's local resharding operation state.
    static const NamespaceString kRecipientReshardingOperationsNamespace;

    // Namespace for persisting sharding DDL coordinators state documents
    static const NamespaceString kShardingDDLCoordinatorsNamespace;

    // Namespace for persisting sharding DDL rename participant state documents
    static const NamespaceString kShardingRenameParticipantsNamespace;

    // Namespace for balancer settings and default read and write concerns.
    static const NamespaceString kConfigSettingsNamespace;

    // Namespace for vector clock state.
    static const NamespaceString kVectorClockNamespace;

    // Namespace for storing oplog applier progress for resharding.
    static const NamespaceString kReshardingApplierProgressNamespace;

    // Namespace for storing config.transactions cloner progress for resharding.
    static const NamespaceString kReshardingTxnClonerProgressNamespace;

    // Namespace for storing config.collectionCriticalSections documents
    static const NamespaceString kCollectionCriticalSectionsNamespace;

    // Dummy namespace used for forcing secondaries to handle an oplog entry on its own batch.
    static const NamespaceString kForceOplogBatchBoundaryNamespace;

    // Namespace used for storing retryable findAndModify images.
    static const NamespaceString kConfigImagesNamespace;

    // Namespace used for persisting ConfigsvrCoordinator state documents.
    static const NamespaceString kConfigsvrCoordinatorsNamespace;

    // Namespace for storing user write blocking critical section documents
    static const NamespaceString kUserWritesCriticalSectionsNamespace;

    // Namespace used during the recovery procedure for the config server.
    static const NamespaceString kConfigsvrRestoreNamespace;

    // Namespace used for CompactParticipantCoordinator service.
    static const NamespaceString kCompactStructuredEncryptionCoordinatorNamespace;

    // Namespace used for storing cluster wide parameters on dedicated configurations.
    static const NamespaceString kClusterParametersNamespace;

    // Namespace used for storing the list of shards on the CSRS.
    static const NamespaceString kConfigsvrShardsNamespace;

    // Namespace used for storing the index catalog on the CSRS.
    static const NamespaceString kConfigsvrIndexCatalogNamespace;

    // Namespace used for storing the index catalog on the shards.
    static const NamespaceString kShardIndexCatalogNamespace;

    // Namespace used for storing the collection catalog on the shards.
    static const NamespaceString kShardCollectionCatalogNamespace;

    // Namespace used for storing NamespacePlacementType docs on the CSRS.
    static const NamespaceString kConfigsvrPlacementHistoryNamespace;

    // TODO SERVER-68551: remove once 7.0 becomes last-lts
    static const NamespaceString kLockpingsNamespace;

    // TODO SERVER-68551: remove once 7.0 becomes last-lts
    static const NamespaceString kDistLocksNamepsace;

    // Namespace used to store the state document of 'SetChangeStreamStateCoordinator'.
    static const NamespaceString kSetChangeStreamStateCoordinatorNamespace;

    /**
     * Constructs an empty NamespaceString.
     */
    NamespaceString() = default;

    /**
     * Constructs a NamespaceString from the fully qualified namespace named in "ns" and the
     * tenantId. "ns" is NOT expected to contain the tenantId.
     */
    explicit NamespaceString(boost::optional<TenantId> tenantId, StringData ns) {
        _dotIndex = ns.find(".");

        uassert(ErrorCodes::InvalidNamespace,
                "namespaces cannot have embedded null characters",
                ns.find('\0') == std::string::npos);

        StringData db = ns.substr(0, _dotIndex);
        _dbName = DatabaseName(std::move(tenantId), db);
        _ns = ns.toString();
    }


    /**
     * Constructs a NamespaceString for the given database.
     */
    explicit NamespaceString(DatabaseName dbName) : _dbName(std::move(dbName)), _ns(_dbName.db()) {}

    // TODO SERVER-65920 Remove this constructor once all constructor call sites have been updated
    // to pass tenantId explicitly
    explicit NamespaceString(StringData ns, boost::optional<TenantId> tenantId = boost::none)
        : NamespaceString(std::move(tenantId), ns) {}

    /**
     * Constructs a NamespaceString for the given database and collection names.
     * "dbName" must not contain a ".", and "collectionName" must not start with one.
     */
    NamespaceString(DatabaseName dbName, StringData collectionName)
        : _dbName(std::move(dbName)), _ns(str::stream() << _dbName.db() << '.' << collectionName) {
        auto db = _dbName.db();

        uassert(ErrorCodes::InvalidNamespace,
                "'.' is an invalid character in the database name: " + db,
                db.find('.') == std::string::npos);
        uassert(ErrorCodes::InvalidNamespace,
                "Collection names cannot start with '.': " + collectionName,
                collectionName.empty() || collectionName[0] != '.');

        _dotIndex = db.size();
        dassert(_ns[_dotIndex] == '.');

        uassert(ErrorCodes::InvalidNamespace,
                "namespaces cannot have embedded null characters",
                _ns.find('\0') == std::string::npos);
    }

    /**
     * Constructs a NamespaceString for the given db name, collection name, and tenantId.
     * "db" must not contain a ".", and "collectionName" must not start with one. "dbName" is
     * NOT expected to contain a tenantId.
     */
    NamespaceString(boost::optional<TenantId> tenantId, StringData db, StringData collectionName)
        : NamespaceString(DatabaseName(std::move(tenantId), db), collectionName) {}

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
    static NamespaceString parseFromStringExpectTenantIdInMultitenancyMode(StringData ns);

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
     * NOTE: DollarInDbNameBehavior::allow is deprecated.
     *
     * Please use DollarInDbNameBehavior::disallow and check explicitly for any DB names that must
     * contain a $.
     */
    enum class DollarInDbNameBehavior {
        Disallow,
        Allow,  // Deprecated
    };

    const boost::optional<TenantId>& tenantId() const {
        return _dbName.tenantId();
    }

    StringData db() const {
        // TODO SERVER-65456 Remove this function.
        return _dbName.db();
    }

    const DatabaseName& dbName() const {
        return _dbName;
    }

    StringData coll() const {
        return _dotIndex == std::string::npos
            ? StringData()
            : StringData(_ns.c_str() + _dotIndex + 1, _ns.size() - 1 - _dotIndex);
    }

    const std::string& ns() const {
        return _ns;
    }

    const std::string& toString() const {
        return ns();
    }

    std::string toStringWithTenantId() const {
        if (auto tenantId = _dbName.tenantId())
            return str::stream() << *tenantId << '_' << ns();

        return ns();
    }

    size_t size() const {
        return _ns.size();
    }

    bool isEmpty() const {
        return _ns.empty();
    }

    //
    // The following methods assume isValid() is true for this NamespaceString.
    //

    bool isHealthlog() const {
        return isLocal() && coll() == "system.healthlog";
    }
    bool isSystem() const {
        return coll().startsWith("system.");
    }
    bool isNormalCollection() const {
        return !isSystem() && !(isLocal() && coll().startsWith("replset."));
    }
    bool isAdminDB() const {
        return db() == kAdminDb;
    }
    bool isLocal() const {
        return db() == kLocalDb;
    }
    bool isSystemDotProfile() const {
        return coll() == "system.profile";
    }
    bool isSystemDotViews() const {
        return coll() == kSystemDotViewsCollectionName;
    }
    bool isSystemDotJavascript() const {
        return coll() == kSystemDotJavascriptCollectionName;
    }
    bool isServerConfigurationCollection() const {
        return (db() == kAdminDb) && (coll() == "system.version");
    }
    bool isPrivilegeCollection() const {
        if (!isAdminDB()) {
            return false;
        }
        return (coll() == kSystemUsers) || (coll() == kSystemRoles);
    }
    bool isConfigDB() const {
        return db() == kConfigDb;
    }
    bool isCommand() const {
        return coll() == "$cmd";
    }
    bool isOplog() const {
        return oplog(_ns);
    }
    bool isOnInternalDb() const {
        if (db() == kAdminDb)
            return true;
        if (db() == kLocalDb)
            return true;
        if (db() == kConfigDb)
            return true;
        return false;
    }

    bool isOrphanCollection() const {
        return db() == kOrphanCollectionDb && coll().startsWith(kOrphanCollectionPrefix);
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

    // Relops among `NamespaceString`.
    friend bool operator==(const NamespaceString& a, const NamespaceString& b) {
        return (a.tenantId() == b.tenantId()) && (a.ns() == b.ns());
    }
    friend bool operator!=(const NamespaceString& a, const NamespaceString& b) {
        return !(a == b);
    }
    friend bool operator<(const NamespaceString& a, const NamespaceString& b) {
        if (a.tenantId() != b.tenantId()) {
            return a.tenantId() < b.tenantId();
        }
        return a.ns() < b.ns();
    }
    friend bool operator>(const NamespaceString& a, const NamespaceString& b) {
        if (a.tenantId() != b.tenantId()) {
            return a.tenantId() > b.tenantId();
        }
        return a.ns() > b.ns();
    }
    friend bool operator<=(const NamespaceString& a, const NamespaceString& b) {
        return !(a > b);
    }
    friend bool operator>=(const NamespaceString& a, const NamespaceString& b) {
        return !(a < b);
    }

    template <typename H>
    friend H AbslHashValue(H h, const NamespaceString& nss) {
        if (nss.tenantId()) {
            return H::combine(std::move(h), nss._dbName.tenantId().get(), nss._ns);
        }
        return H::combine(std::move(h), nss._ns);
    }

    friend auto logAttrs(const NamespaceString& nss) {
        return "namespace"_attr = nss;
    }

private:
    DatabaseName _dbName;
    std::string _ns;
    size_t _dotIndex = std::string::npos;
};

/**
 * This class is intented to be used by commands which can accept either a collection name or
 * database + collection UUID. It will never be initialized with both.
 */
class NamespaceStringOrUUID {
public:
    NamespaceStringOrUUID() = delete;
    NamespaceStringOrUUID(NamespaceString nss) : _nss(std::move(nss)) {}
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
        return _dbname ? _dbname->db() : "";
    }

    const boost::optional<DatabaseName>& dbName() const {
        return _dbname;
    }

    void preferNssForSerialization() {
        _preferNssForSerialization = true;
    }

    /**
     * Returns database name derived from either '_nss' or '_dbname'.
     */
    StringData db() const {
        return _nss ? _nss->db() : StringData(_dbname->db());
    }

    /**
     * Returns OK if either the nss is not set or is a valid nss. Otherwise returns an
     * InvalidNamespace error.
     */
    Status isNssValid() const;

    std::string toString() const;

    void serialize(BSONObjBuilder* builder, StringData fieldName) const;

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

std::ostream& operator<<(std::ostream& stream, const NamespaceString& nss);
std::ostream& operator<<(std::ostream& stream, const NamespaceStringOrUUID& nsOrUUID);
StringBuilder& operator<<(StringBuilder& builder, const NamespaceString& nss);
StringBuilder& operator<<(StringBuilder& builder, const NamespaceStringOrUUID& nsOrUUID);

/**
 * "database.a.b.c" -> "database"
 */
inline StringData nsToDatabaseSubstring(StringData ns) {
    size_t i = ns.find('.');
    if (i == std::string::npos) {
        massert(
            10078, "nsToDatabase: db too long", ns.size() < NamespaceString::MaxDatabaseNameLen);
        return ns;
    }
    massert(10088,
            "nsToDatabase: db too long",
            i < static_cast<size_t>(NamespaceString::MaxDatabaseNameLen));
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
    if (db.size() == 0 || db.size() >= 64)
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

}  // namespace mongo
