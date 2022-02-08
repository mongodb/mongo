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
#include "mongo/db/repl/optime.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/uuid.h"

namespace mongo {

const size_t MaxDatabaseNameLen = 128;  // max str len for the db name, including null char

class NamespaceString {
public:
    // Reserved system namespaces

    // Namespace for the admin database
    static constexpr StringData kAdminDb = "admin"_sd;

    // Namespace for the local database
    static constexpr StringData kLocalDb = "local"_sd;

    // Namespace for the sharding config database
    static constexpr StringData kConfigDb = "config"_sd;

    // Name for the system views collection
    static constexpr StringData kSystemDotViewsCollectionName = "system.views"_sd;

    // Prefix for orphan collections
    static constexpr StringData kOrphanCollectionPrefix = "orphan."_sd;
    static constexpr StringData kOrphanCollectionDb = "local"_sd;

    // Namespace for storing configuration data, which needs to be replicated if the server is
    // running as a replica set. Documents in this collection should represent some configuration
    // state of the server, which needs to be recovered/consulted at startup. Each document in this
    // namespace should have its _id set to some string, which meaningfully describes what it
    // represents. For example, 'shardIdentity' and 'featureCompatibilityVersion'.
    static const NamespaceString kServerConfigurationNamespace;

    // Namespace for storing the logical sessions information
    static const NamespaceString kLogicalSessionsNamespace;

    // Namespace for storing the transaction information for each session
    static const NamespaceString kSessionTransactionsTableNamespace;

    // Name for a shard's collections metadata collection, each document of which indicates the
    // state of a specific collection
    static const NamespaceString kShardConfigCollectionsNamespace;

    // Name for a shard's databases metadata collection, each document of which indicates the state
    // of a specific database
    static const NamespaceString kShardConfigDatabasesNamespace;

    // Name for causal consistency's key collection.
    static const NamespaceString kSystemKeysNamespace;

    // Namespace of the the oplog collection.
    static const NamespaceString kRsOplogNamespace;

    // Namespace for storing the persisted state of transaction coordinators.
    static const NamespaceString kTransactionCoordinatorsNamespace;

    // Namespace for replica set configuration settings.
    static const NamespaceString kSystemReplSetNamespace;

    // Namespace for index build entries.
    static const NamespaceString kIndexBuildEntryNamespace;

    // Namespace used for storing retryable findAndModify images.
    static const NamespaceString kConfigImagesNamespace;

    /**
     * Constructs an empty NamespaceString.
     */
    NamespaceString() : _ns(), _dotIndex(std::string::npos) {}

    /**
     * Constructs a NamespaceString from the fully qualified namespace named in "ns".
     */
    explicit NamespaceString(StringData ns) {
        _ns = ns.toString();  // copy to our buffer
        _dotIndex = _ns.find('.');
        uassert(ErrorCodes::InvalidNamespace,
                "namespaces cannot have embedded null characters",
                _ns.find('\0') == std::string::npos);
    }

    /**
     * Constructs a NamespaceString for the given database and collection names.
     * "dbName" must not contain a ".", and "collectionName" must not start with one.
     */
    NamespaceString(StringData dbName, StringData collectionName)
        : _ns(dbName.size() + collectionName.size() + 1, '\0') {
        uassert(ErrorCodes::InvalidNamespace,
                "'.' is an invalid character in a database name",
                dbName.find('.') == std::string::npos);
        uassert(ErrorCodes::InvalidNamespace,
                "Collection names cannot start with '.'",
                collectionName.empty() || collectionName[0] != '.');

        std::string::iterator it = std::copy(dbName.begin(), dbName.end(), _ns.begin());
        *it = '.';
        ++it;
        it = std::copy(collectionName.begin(), collectionName.end(), it);
        _dotIndex = dbName.size();

        dassert(it == _ns.end());
        dassert(_ns[_dotIndex] == '.');

        uassert(ErrorCodes::InvalidNamespace,
                "namespaces cannot have embedded null characters",
                _ns.find('\0') == std::string::npos);
    }

    /**
     * Constructs the namespace '<dbName>.$cmd.aggregate', which we use as the namespace for
     * aggregation commands with the format {aggregate: 1}.
     */
    static NamespaceString makeCollectionlessAggregateNSS(StringData dbName);

    /**
     * Constructs a NamespaceString representing a listCollections namespace. The format for this
     * namespace is "<dbName>.$cmd.listCollections".
     */
    static NamespaceString makeListCollectionsNSS(StringData dbName);

    /**
     * Note that these values are derived from the mmap_v1 implementation and that is the only
     * reason they are constrained as such.
     */
    enum MaxNsLenValue {
        // Maximum possible length of name any namespace, including special ones like $extra.
        // This includes rum for the NUL byte so it can be used when sizing buffers.
        MaxNsLenWithNUL = 128,

        // MaxNsLenWithNUL excluding the NUL byte. Use this when comparing std::string lengths.
        MaxNsLen = MaxNsLenWithNUL - 1,

        // Maximum allowed length of fully qualified namespace name of any real collection.
        // Does not include NUL so it can be directly compared to std::string lengths.
        MaxNsCollectionLen = MaxNsLen - 7 /*strlen(".$extra")*/,

        // The maximum namespace length of sharded collections is less than that of unsharded ones
        // since the namespace of the cached chunks metadata, local to each shard, is composed by
        // the namespace of the related sharded collection (i.e., config.cache.chunks.<ns>).
        MaxNsShardedCollectionLen = 100 /* MaxNsCollectionLen - len(ChunkType::ShardNSPrefix) */,
    };

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

    StringData db() const {
        return _dotIndex == std::string::npos ? _ns : StringData(_ns.data(), _dotIndex);
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
    bool isServerConfigurationCollection() const {
        return (db() == kAdminDb) && (coll() == "system.version");
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
    bool isSpecial() const {
        return special(_ns);
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
    bool isNormal() const {
        return normal(_ns);
    }
    bool isOrphanCollection() const {
        return db() == kOrphanCollectionDb && coll().startsWith(kOrphanCollectionPrefix);
    }

    /**
     * Returns whether the NamespaceString references a special collection that cannot be used for
     * generic data storage.
     */
    bool isVirtualized() const {
        return virtualized(_ns);
    }

    /**
     * Returns whether a namespace is replicated, based only on its string value. One notable
     * omission is that map reduce `tmp.mr` collections may or may not be replicated. Callers must
     * decide how to handle that case separately.
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

    bool isCollectionlessAggregateNS() const;
    bool isListCollectionsCursorNS() const;

    /**
     * Returns true if a client can modify this namespace even though it is under ".system."
     * For example <dbname>.system.users is ok for regular clients to update.
     */
    bool isLegalClientSystemNS() const;

    /**
     * Returns true if this namespace refers to a drop-pending collection.
     */
    bool isDropPendingNamespace() const;

    /**
     * Returns the drop-pending namespace name for this namespace, provided the given optime.
     *
     * Example:
     *     test.foo -> test.system.drop.<timestamp seconds>i<timestamp increment>t<term>.foo
     *
     * Original collection name may be truncated so that the generated namespace length does not
     * exceed MaxNsCollectionLen.
     */
    NamespaceString makeDropPendingNamespace(const repl::OpTime& opTime) const;

    /**
     * Returns the optime used to generate the drop-pending namespace.
     * Returns an error if this namespace is not drop-pending.
     */
    StatusWith<repl::OpTime> getDropPendingNamespaceOpTime() const;

    /**
     * Checks if this namespace is valid as a target namespace for a rename operation, given
     * the length of the longest index name in the source collection.
     */
    Status checkLengthForRename(const std::string::size_type longestIndexNameLength) const;

    /**
     * Returns true if the namespace is valid. Special namespaces for internal use are considered as
     * valid.
     */
    bool isValid() const {
        return validDBName(db(), DollarInDbNameBehavior::Allow) && !coll().empty();
    }

    /**
     * NamespaceString("foo.bar").getSisterNS("blah") returns "foo.blah".
     */
    std::string getSisterNS(StringData local) const;

    NamespaceString getCommandNS() const {
        return {db(), "$cmd"};
    }

    /**
     * Returns index namespace for an index in this collection namespace.
     */
    NamespaceString makeIndexNamespace(StringData indexName) const;

    /**
     * @return true if ns is 'normal'.  A "$" is used for namespaces holding index data,
     * which do not contain BSON objects in their records. ("oplog.$main" is the exception)
     */
    static bool normal(StringData ns) {
        return !virtualized(ns);
    }

    /**
     * @return true if the ns is an oplog one, otherwise false.
     */
    static bool oplog(StringData ns) {
        return ns.startsWith("local.oplog.");
    }

    static bool special(StringData ns) {
        return !normal(ns) || ns.substr(ns.find('.')).startsWith(".system.");
    }

    /**
     * Check if `ns` references a special collection that cannot be used for generic data storage.
     */
    static bool virtualized(StringData ns) {
        return ns.find('$') != std::string::npos && ns != "local.oplog.$main";
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
    static bool validDBName(StringData db,
                            DollarInDbNameBehavior behavior = DollarInDbNameBehavior::Disallow);

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
        return a.ns() == b.ns();
    }
    friend bool operator!=(const NamespaceString& a, const NamespaceString& b) {
        return a.ns() != b.ns();
    }
    friend bool operator<(const NamespaceString& a, const NamespaceString& b) {
        return a.ns() < b.ns();
    }
    friend bool operator>(const NamespaceString& a, const NamespaceString& b) {
        return a.ns() > b.ns();
    }
    friend bool operator<=(const NamespaceString& a, const NamespaceString& b) {
        return a.ns() <= b.ns();
    }
    friend bool operator>=(const NamespaceString& a, const NamespaceString& b) {
        return a.ns() >= b.ns();
    }

    template <typename H>
    friend H AbslHashValue(H h, const NamespaceString& nss) {
        return H::combine(std::move(h), nss._ns);
    }

private:
    std::string _ns;
    size_t _dotIndex;
};

/**
 * This class is intented to be used by commands which can accept either a collection name or
 * database + collection UUID. It will never be initialized with both.
 */
class NamespaceStringOrUUID {
public:
    NamespaceStringOrUUID(NamespaceString nss) : _nss(std::move(nss)) {}
    NamespaceStringOrUUID(std::string dbname, UUID uuid)
        : _uuid(std::move(uuid)), _dbname(std::move(dbname)) {}

    const boost::optional<NamespaceString>& nss() const {
        return _nss;
    }

    const boost::optional<UUID>& uuid() const {
        return _uuid;
    }

    const std::string& dbname() const {
        return _dbname;
    }

    std::string toString() const;

private:
    // At any given time exactly one of these optionals will be initialized
    boost::optional<NamespaceString> _nss;
    boost::optional<UUID> _uuid;

    // Empty string when '_nss' is non-none, and contains the database name when '_uuid' is
    // non-none. Although the UUID specifies a collection uniquely, we must later verify that the
    // collection belongs to the database named here.
    std::string _dbname;
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
        massert(10078, "nsToDatabase: db too long", ns.size() < MaxDatabaseNameLen);
        return ns;
    }
    massert(10088, "nsToDatabase: db too long", i < static_cast<size_t>(MaxDatabaseNameLen));
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
