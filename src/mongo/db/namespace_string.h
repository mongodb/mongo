/**
 *    Copyright (C) 2017 MongoDB, Inc.
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
 *    must comply with the GNU Affero General Public License in all respects for
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
#include "mongo/db/repl/optime.h"
#include "mongo/platform/hash_namespace.h"
#include "mongo/util/assert_util.h"

namespace mongo {

const size_t MaxDatabaseNameLen = 128;  // max str len for the db name, including null char

/* e.g.
   NamespaceString ns("acme.orders");
   cout << ns.coll; // "orders"
*/
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

    // Name for a shard's collections metadata collection, each document of which indicates the
    // state of a specific collection.
    static constexpr StringData kShardConfigCollectionsCollectionName = "config.collections"_sd;

    // Namespace for storing configuration data, which needs to be replicated if the server is
    // running as a replica set. Documents in this collection should represent some configuration
    // state of the server, which needs to be recovered/consulted at startup. Each document in this
    // namespace should have its _id set to some string, which meaningfully describes what it
    // represents.
    static const NamespaceString kServerConfigurationNamespace;

    // Namespace for storing the transaction information for each session
    static const NamespaceString kSessionTransactionsTableNamespace;

    // Namespace of the the oplog collection.
    static const NamespaceString kRsOplogNamespace;

    /**
     * Constructs an empty NamespaceString.
     */
    NamespaceString();

    /**
     * Constructs a NamespaceString from the fully qualified namespace named in "ns".
     */
    explicit NamespaceString(StringData ns);

    /**
     * Constructs a NamespaceString for the given database and collection names.
     * "dbName" must not contain a ".", and "collectionName" must not start with one.
     */
    NamespaceString(StringData dbName, StringData collectionName);

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
     * Constructs a NamespaceString representing a listIndexes namespace. The format for this
     * namespace is "<dbName>.$cmd.listIndexes.<collectionName>".
     */
    static NamespaceString makeListIndexesNSS(StringData dbName, StringData collectionName);

    /**
     * Note that these values are derived from the mmap_v1 implementation and that
     * is the only reason they are constrained as such.
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
    };

    /**
     * DollarInDbNameBehavior::allow is deprecated.
     * Please use DollarInDbNameBehavior::disallow and check explicitly for any DB names that must
     * contain a $.
     */
    enum class DollarInDbNameBehavior {
        Disallow,
        Allow,  // Deprecated
    };

    StringData db() const;
    StringData coll() const;

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

    struct Hasher {
        size_t operator()(const NamespaceString& nss) const {
            return std::hash<std::string>()(nss._ns);
        }
    };

    //
    // The following methods assume isValid() is true for this NamespaceString.
    //

    bool isSystem() const {
        return coll().startsWith("system.");
    }
    bool isLocal() const {
        return db() == "local";
    }
    bool isSystemDotIndexes() const {
        return coll() == "system.indexes";
    }
    bool isSystemDotProfile() const {
        return coll() == "system.profile";
    }
    bool isSystemDotViews() const {
        return coll() == kSystemDotViewsCollectionName;
    }
    bool isConfigDB() const {
        return db() == "config";
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
        return internalDb(db());
    }
    bool isNormal() const {
        return normal(_ns);
    }

    // Check if the NamespaceString references a special collection that cannot
    // be used for generic data storage.
    bool isVirtualized() const {
        return virtualized(_ns);
    }

    /**
     * Returns true if cursors for this namespace are registered with the global cursor manager.
     */
    bool isGloballyManagedNamespace() const {
        return coll().startsWith("$cmd."_sd);
    }

    bool isCollectionlessAggregateNS() const;
    bool isListCollectionsCursorNS() const;
    bool isListIndexesCursorNS() const;

    /**
     * Returns true if a client can modify this namespace even though it is under ".system."
     * For example <dbname>.system.users is ok for regular clients to update.
     */
    bool isLegalClientSystemNS() const;

    /**
     * Given a NamespaceString for which isGloballyManagedNamespace() returns true, returns the
     * namespace the command targets, or boost::none for commands like 'listCollections' which
     * do not target a collection.
     */
    boost::optional<NamespaceString> getTargetNSForGloballyManagedNamespace() const;

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
     * Given a NamespaceString for which isListIndexesCursorNS() returns true, returns the
     * NamespaceString for the collection that the "listIndexes" targets.
     */
    NamespaceString getTargetNSForListIndexes() const;

    /**
     * @return true if the namespace is valid. Special namespaces for internal use are considered as
     * valid.
     */
    bool isValid() const {
        return validDBName(db(), DollarInDbNameBehavior::Allow) && !coll().empty();
    }

    bool operator==(const std::string& nsIn) const {
        return nsIn == _ns;
    }
    bool operator==(StringData nsIn) const {
        return nsIn == _ns;
    }
    bool operator==(const NamespaceString& nsIn) const {
        return nsIn._ns == _ns;
    }

    bool operator!=(const std::string& nsIn) const {
        return nsIn != _ns;
    }
    bool operator!=(const NamespaceString& nsIn) const {
        return nsIn._ns != _ns;
    }

    bool operator<(const NamespaceString& rhs) const {
        return _ns < rhs._ns;
    }

    /** ( foo.bar ).getSisterNS( "blah" ) == foo.blah
     */
    std::string getSisterNS(StringData local) const;

    // @return db() + ".system.indexes"
    std::string getSystemIndexesCollection() const;

    // @return {db(), "$cmd"}
    NamespaceString getCommandNS() const;

    /**
     * Function to escape most non-alpha characters from file names
     */
    static std::string escapeDbName(const StringData dbname);

    /**
     * @return true if ns is 'normal'.  A "$" is used for namespaces holding index data,
     * which do not contain BSON objects in their records. ("oplog.$main" is the exception)
     */
    static bool normal(StringData ns);

    /**
     * @return true if the ns is an oplog one, otherwise false.
     */
    static bool oplog(StringData ns);

    static bool special(StringData ns);

    // Check if `ns` references a special collection that cannot be used for
    // generic data storage.
    static bool virtualized(StringData ns);

    /**
     * Returns true for DBs with special meaning to mongodb.
     */
    static bool internalDb(StringData db) {
        if (db == "admin")
            return true;
        if (db == "local")
            return true;
        if (db == "config")
            return true;
        return false;
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
     *     system.indexes
     *   bad:
     *     $foo
     * @param coll - a collection name component of a namespace
     * @return if the input is a valid collection name
     */
    static bool validCollectionName(StringData coll);

private:
    std::string _ns;
    size_t _dotIndex;
};

std::ostream& operator<<(std::ostream& stream, const NamespaceString& nss);

// "database.a.b.c" -> "database"
inline StringData nsToDatabaseSubstring(StringData ns) {
    size_t i = ns.find('.');
    if (i == std::string::npos) {
        massert(10078, "nsToDatabase: db too long", ns.size() < MaxDatabaseNameLen);
        return ns;
    }
    massert(10088, "nsToDatabase: db too long", i < static_cast<size_t>(MaxDatabaseNameLen));
    return ns.substr(0, i);
}

// "database.a.b.c" -> "database"
inline void nsToDatabase(StringData ns, char* database) {
    StringData db = nsToDatabaseSubstring(ns);
    db.copyTo(database, true);
}

// TODO: make this return a StringData
inline std::string nsToDatabase(StringData ns) {
    return nsToDatabaseSubstring(ns).toString();
}

// "database.a.b.c" -> "a.b.c"
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

/**
 * this can change, do not store on disk
 */
int nsDBHash(const std::string& ns);

}  // namespace mongo

#include "mongo/db/namespace_string-inl.h"

MONGO_HASH_NAMESPACE_START
template <>
struct hash<mongo::NamespaceString> {
    size_t operator()(const mongo::NamespaceString& nss) const {
        mongo::NamespaceString::Hasher hasher;
        return hasher(nss);
    }
};
MONGO_HASH_NAMESPACE_END
