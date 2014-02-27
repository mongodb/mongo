/**
*    Copyright (C) 2008 10gen Inc.
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

#include "mongo/pch.h"

#include <boost/filesystem/convenience.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/scoped_ptr.hpp>
#include <fcntl.h>
#include <fstream>
#include <set>

#include "mongo/bson/util/bson_extract.h"
#include "mongo/client/auth_helpers.h"
#include "mongo/client/dbclientcursor.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/auth/role_name.h"
#include "mongo/db/json.h"
#include "mongo/db/namespace_string.h"
#include "mongo/tools/mongorestore_options.h"
#include "mongo/tools/tool.h"
#include "mongo/util/mmap.h"
#include "mongo/util/options_parser/option_section.h"
#include "mongo/util/stringutils.h"

using namespace mongo;

namespace {
    const char* OPLOG_SENTINEL = "$oplog";  // compare by ptr not strcmp
}

class Restore : public BSONTool {
public:

    string _curns;
    string _curdb;
    string _curcoll;
    string _userDBFieldName;
    set<UserName> _users; // Holds users that are already in the cluster when restoring with --drop
    set<RoleName> _roles; // Holds roles that are already in the cluster when restoring with --drop
    scoped_ptr<Matcher> _opmatcher; // For oplog replay
    scoped_ptr<OpTime> _oplogLimitTS; // for oplog replay (limit)
    int _oplogEntrySkips; // oplog entries skipped
    int _oplogEntryApplies; // oplog entries applied
    int _serverAuthzVersion; // authSchemaVersion of the cluster being restored into.
    int _dumpFileAuthzVersion; // version extracted from admin.system.version file in dump.
    bool _serverAuthzVersionDocExists; // Whether the remote cluster has an admin.system.version doc
    Restore() : BSONTool() { }

    virtual void printHelp(ostream& out) {
        printMongoRestoreHelp(&out);
    }

    void storeRemoteAuthzVersion() {
        Status status = auth::getRemoteStoredAuthorizationVersion(&conn(),
                                                                  &_serverAuthzVersion);
        uassertStatusOK(status);
        uassert(17370,
                mongoutils::str::stream() << "Restoring users and roles is only supported for "
                        "clusters with auth schema versions " <<
                        AuthorizationManager::schemaVersion24 << " or " <<
                        AuthorizationManager::schemaVersion26Final << ", found: " <<
                        _serverAuthzVersion,
                _serverAuthzVersion == AuthorizationManager::schemaVersion24 ||
                _serverAuthzVersion == AuthorizationManager::schemaVersion26Final);

        _serverAuthzVersionDocExists = !conn().findOne(
                AuthorizationManager::versionCollectionNamespace,
                AuthorizationManager::versionDocumentQuery).isEmpty();

        // Now that we know the schema version of the server we can pick whether to use
        // "userSource" or "db" to identify users.
        _userDBFieldName = _serverAuthzVersion == AuthorizationManager::schemaVersion26Final ?
                "db" : "userSource";
    }

    virtual int doRun() {

        boost::filesystem::path root = mongoRestoreGlobalParams.restoreDirectory;

        // check if we're actually talking to a machine that can write
        if (!isMaster()) {
            return -1;
        }

        if (isMongos() && toolGlobalParams.db == "" && exists(root / "config")) {
            toolError() << "Cannot do a full restore on a sharded system" << std::endl;
            return -1;
        }

        if (mongoRestoreGlobalParams.restoreUsersAndRoles) {
            storeRemoteAuthzVersion(); // populate _serverAuthzVersion

            if (toolGlobalParams.db.empty() && toolGlobalParams.coll.empty() &&
                    exists(root / "admin" / "system.version.bson")) {
                // Will populate _dumpFileAuthzVersion
                processFileAndMetadata(root / "admin" / "system.version.bson",
                                       "admin.system.version");
                uassert(17371,
                        mongoutils::str::stream() << "Server's authorization data schema version "
                                "does not match that of the data in the dump file.  Server's schema"
                                " version: " << _serverAuthzVersion << ", schema version in dump: "
                                << _dumpFileAuthzVersion,
                        _serverAuthzVersion == _dumpFileAuthzVersion);
            } else if (!toolGlobalParams.db.empty()) {
                // DB-specific restore
                if (exists(root / "$admin.system.users.bson")) {
                    uassert(17372,
                            mongoutils::str::stream() << "$admin.system.users.bson file found, "
                                    "which implies that the dump was taken from a system with "
                                    "schema version " << AuthorizationManager::schemaVersion26Final
                                    << " users, but server has authorization schema version "
                                    << _serverAuthzVersion,
                            _serverAuthzVersion == AuthorizationManager::schemaVersion26Final);
                    toolInfoLog() << "Restoring users for the " << toolGlobalParams.db <<
                            " database to admin.system.users" << endl;
                    processFileAndMetadata(root / "$admin.system.users.bson", "admin.system.users");
                }
                if (exists(root / "$admin.system.roles.bson")) {
                    uassert(17373,
                            mongoutils::str::stream() << "$admin.system.roles.bson file found, "
                                    "which implies that the dump was taken from a system with  "
                                    "schema version " << AuthorizationManager::schemaVersion26Final
                                    << " authorization data, but server has authorization schema "
                                    "version " << _serverAuthzVersion,
                            _serverAuthzVersion == AuthorizationManager::schemaVersion26Final);
                    toolInfoLog() << "Restoring roles for the " << toolGlobalParams.db <<
                            " database to admin.system.roles" << endl;
                    processFileAndMetadata(root / "$admin.system.roles.bson", "admin.system.roles");
                }
            }
        }

        if (mongoRestoreGlobalParams.oplogReplay) {
            // fail early if errors

            if (toolGlobalParams.db != "") {
                toolError() << "Can only replay oplog on full restore" << std::endl;
                return -1;
            }

            if ( ! exists(root / "oplog.bson") ) {
                toolError() << "No oplog file to replay. Make sure you run mongodump with --oplog."
                          << std::endl;
                return -1;
            }


            BSONObj out;
            if (! conn().simpleCommand("admin", &out, "buildinfo")) {
                toolError() << "buildinfo command failed: " << out["errmsg"].String() << std::endl;
                return -1;
            }

            StringData version = out["version"].valuestr();
            if (versionCmp(version, "1.7.4-pre-") < 0) {
                toolError() << "Can only replay oplog to server version >= 1.7.4" << std::endl;
                return -1;
            }

            string oplogInc = "0";

            if(!mongoRestoreGlobalParams.oplogLimit.empty()) {
                size_t i = mongoRestoreGlobalParams.oplogLimit.find_first_of(':');
                if ( i != string::npos ) {
                    if (i + 1 < mongoRestoreGlobalParams.oplogLimit.length()) {
                        oplogInc = mongoRestoreGlobalParams.oplogLimit.substr(i + 1);
                    }

                    mongoRestoreGlobalParams.oplogLimit =
                        mongoRestoreGlobalParams.oplogLimit.substr(0, i);
                }

                try {
                    _oplogLimitTS.reset(new OpTime(
                        boost::lexical_cast<unsigned long>(
                            mongoRestoreGlobalParams.oplogLimit.c_str()),
                        boost::lexical_cast<unsigned long>(oplogInc.c_str())));
                } catch( const boost::bad_lexical_cast& ) {
                    toolError() << "Could not parse oplogLimit into Timestamp from values ( "
                              << mongoRestoreGlobalParams.oplogLimit << " , " << oplogInc << " )"
                              << std::endl;
                    return -1;
                }

                if (!mongoRestoreGlobalParams.oplogLimit.empty()) {
                    // Only for a replica set as master will have no-op entries so we would need to
                    // skip them all to find the real op
                    scoped_ptr<DBClientCursor> cursor(
                            conn().query("local.oplog.rs", Query().sort(BSON("$natural" << -1)),
                                         1 /*return first*/));
                    OpTime tsOptime;
                    // get newest oplog entry and make sure it is older than the limit to apply.
                    if (cursor->more()) {
                        tsOptime = cursor->next().getField("ts")._opTime();
                        if (tsOptime > *_oplogLimitTS.get()) {
                            toolError() << "The oplogLimit is not newer than"
                                      << " the last oplog entry on the server."
                                      << std::endl;
                            return -1;
                        }
                    }

                    BSONObjBuilder tsRestrictBldr;
                    if (!tsOptime.isNull())
                        tsRestrictBldr << "$gt" << tsOptime;
                    tsRestrictBldr << "$lt" << *_oplogLimitTS.get();

                    BSONObj query = BSON("ts" << tsRestrictBldr.obj());

                    if (!tsOptime.isNull()) {
                        toolInfoLog() << "Latest oplog entry on the server is " << tsOptime.getSecs()
                                      << ":" << tsOptime.getInc() << std::endl;
                        toolInfoLog() << "Only applying oplog entries matching this criteria: "
                                      << query.jsonString() << std::endl;
                    }
                    _opmatcher.reset(new Matcher(query));
                }
            }
        }

        /* If toolGlobalParams.db is not "" then the user specified a db name to restore as.
         *
         * In that case we better be given either a root directory that
         * contains only .bson files or a single .bson file  (a db).
         *
         * In the case where a collection name is specified we better be
         * given either a root directory that contains only a single
         * .bson file, or a single .bson file itself (a collection).
         */
        drillDown(root, toolGlobalParams.db != "", toolGlobalParams.coll != "",
                  !(_oplogLimitTS.get() == NULL), true);

        // should this happen for oplog replay as well?
        string err = conn().getLastError(toolGlobalParams.db == "" ? "admin" : toolGlobalParams.db);
        if (!err.empty()) {
            toolError() << err << std::endl;
        }

        if (mongoRestoreGlobalParams.oplogReplay) {
            toolInfoLog() << "\t Replaying oplog" << std::endl;
            _curns = OPLOG_SENTINEL;
            processFile( root / "oplog.bson" );
            toolInfoLog() << "Applied " << _oplogEntryApplies << " oplog entries out of "
                          << _oplogEntryApplies + _oplogEntrySkips << " (" << _oplogEntrySkips
                          << " skipped)." << std::endl;
        }

        return EXIT_CLEAN;
    }

    void drillDown( boost::filesystem::path root,
                    bool use_db,
                    bool use_coll,
                    bool oplogReplayLimit,
                    bool top_level=false) {
        bool json_metadata = false;
        if (logger::globalLogDomain()->shouldLog(logger::LogSeverity::Debug(2))) {
            toolInfoLog() << "drillDown: " << root.string() << std::endl;
        }

        // skip hidden files and directories
        if (root.leaf().string()[0] == '.' && root.leaf().string() != ".")
            return;

        if ( is_directory( root ) ) {
            boost::filesystem::directory_iterator end;
            boost::filesystem::directory_iterator i(root);
            boost::filesystem::path indexes;
            while ( i != end ) {
                boost::filesystem::path p = *i;
                i++;

                if (use_db) {
                    if (boost::filesystem::is_directory(p)) {
                        toolError() << "ERROR: root directory must be a dump of a single database"
                                  << std::endl;
                        toolError() << "       when specifying a db name with --db" << std::endl;
                        toolError() << "       use the --help option for more information"
                                  << std::endl;
                        return;
                    }
                }

                if (use_coll) {
                    if (boost::filesystem::is_directory(p) || i != end) {
                        toolError() << "ERROR: root directory must be a dump of a single collection"
                                  << std::endl;
                        toolError() << "       when specifying a collection name with --collection"
                                  << std::endl;
                        toolError() << "       use the --help option for more information"
                                  << std::endl;
                        return;
                    }
                }

                // Ignore system.indexes.bson if we have *.metadata.json files
                if ( endsWith( p.string().c_str() , ".metadata.json" ) ) {
                    json_metadata = true;
                }

                // don't insert oplog
                if (top_level && !use_db && p.leaf() == "oplog.bson")
                    continue;

                if ( p.leaf() == "system.indexes.bson" ) {
                    indexes = p;
                } else {
                    drillDown(p, use_db, use_coll, oplogReplayLimit);
                }
            }

            if (!indexes.empty() && !json_metadata) {
                drillDown(indexes, use_db, use_coll, oplogReplayLimit);
            }

            return;
        }

        if (oplogReplayLimit) {
            toolError() << "The oplogLimit option cannot be used if "
                      << "normal databases/collections exist in the dump directory."
                      << std::endl;
            exit(EXIT_FAILURE);
        }

        string ns;
        if (use_db) {
            ns += toolGlobalParams.db;
        }
        else {
            ns = root.parent_path().filename().string();
            if (ns.empty())
                ns = "test";
        }

        verify( ns.size() );

        string oldCollName = root.leaf().string(); // Name of the collection that was dumped from
        oldCollName = oldCollName.substr( 0 , oldCollName.find_last_of( "." ) );
        if (use_coll) {
            ns += "." + toolGlobalParams.coll;
        }
        else {
            ns += "." + oldCollName;
        }

        if ( endsWith( root.string().c_str() , ".metadata.json" ) ) {
            // Metadata files are handled when the corresponding .bson file is handled
            return;
        }

        if ((root.leaf() == "system.version.bson" && toolGlobalParams.db.empty()) ||
                root.leaf() == "$admin.system.users.bson" ||
                root.leaf() == "$admin.system.roles.bson") {
            // These files were already explicitly handled at the beginning of the restore.
            return;
        }

        if ( ! ( endsWith( root.string().c_str() , ".bson" ) ||
                 endsWith( root.string().c_str() , ".bin" ) ) ) {
            toolError() << "don't know what to do with file [" << root.string() << "]" << std::endl;
            return;
        }

        toolInfoLog() << root.string() << std::endl;

        if ( root.leaf() == "system.profile.bson" ) {
            toolInfoLog() << "\t skipping system.profile.bson" << std::endl;
            return;
        }

        processFileAndMetadata(root, ns);
    }

    /**
     * 1) Drop collection if --drop was specified.  For system.users or system.roles collections,
     * however, you don't want to remove all the users/roles up front as some of them may be needed
     * by the restore.  Instead, keep a set of all the users/roles originally in the server, then
     * after restoring the users/roles from the dump, remove any users roles that were present in
     * the system originally but aren't in the dump.
     *
     * 2) Parse metadata file (if present) and if the collection doesn't exist (or was just dropped
     * b/c we're using --drop), create the collection with the options from the metadata file
     *
     * 3) Restore the data from the dump file for this collection
     *
     * 4) If the user asked to drop this collection, then at this point the _users and _roles sets
     * will contain users and roles that were in the collection but not in the dump we are
     * restoring. Iterate these sets and delete any users and roles that are there.
     *
     * 5) Restore indexes based on index definitions from the metadata file.
     */
    void processFileAndMetadata(const boost::filesystem::path& root, const std::string& ns) {

        _curns = ns;
        _curdb = nsToDatabase(_curns);
        _curcoll = nsToCollectionSubstring(_curns).toString();

        toolInfoLog() << "\tgoing into namespace [" << _curns << "]" << std::endl;

        // 1) Drop collection if needed.  Save user and role data if this is a system.users or
        // system.roles collection
        if (mongoRestoreGlobalParams.drop) {
            if (_curcoll == "system.users") {
                // Create map of the users currently in the DB
                BSONObj fields = BSON("user" << 1 << _userDBFieldName << 1);
                scoped_ptr<DBClientCursor> cursor(conn().query(_curns, Query(), 0, 0, &fields));
                while (cursor->more()) {
                    BSONObj user = cursor->next();
                    string userDB;
                    uassertStatusOK(bsonExtractStringFieldWithDefault(user,
                                                                      _userDBFieldName,
                                                                      _curdb,
                                                                      &userDB));
                    _users.insert(UserName(user["user"].String(), userDB));
                }
            }
            else if (_curns == "admin.system.roles") {
                // Create map of the roles currently in the DB
                BSONObj fields = BSON("role" << 1 << "db" << 1);
                scoped_ptr<DBClientCursor> cursor(conn().query(_curns, Query(), 0, 0, &fields));
                while (cursor->more()) {
                    BSONObj role = cursor->next();
                    _roles.insert(RoleName(role["role"].String(), role["db"].String()));
                }
            }
            else {
                toolInfoLog() << "\t dropping" << std::endl;
                conn().dropCollection( ns );
            }
        } else {
            // If drop is not used, warn if the collection exists.
            scoped_ptr<DBClientCursor> cursor(conn().query(_curdb + ".system.namespaces",
                                                           Query(BSON("name" << ns))));
            if (cursor->more()) {
                // collection already exists show warning
                toolError() << "Restoring to " << ns << " without dropping. Restored data "
                        << "will be inserted without raising errors; check your server log"
                        << std::endl;
            }
        }

        // 2) Create collection with options from metadata file if present
        BSONObj metadataObject;
        if (mongoRestoreGlobalParams.restoreOptions || mongoRestoreGlobalParams.restoreIndexes) {
            string oldCollName = root.leaf().string(); // Name of collection that was dumped from
            oldCollName = oldCollName.substr( 0 , oldCollName.find_last_of( "." ) );
            boost::filesystem::path metadataFile = (root.branch_path() / (oldCollName + ".metadata.json"));
            if (!boost::filesystem::exists(metadataFile.string())) {
                // This is fine because dumps from before 2.1 won't have a metadata file, just print a warning.
                // System collections shouldn't have metadata so don't warn if that file is missing.
                if (!startsWith(metadataFile.leaf().string(), "system.")) {
                    toolInfoLog() << metadataFile.string() << " not found. Skipping." << std::endl;
                }
            } else {
                metadataObject = parseMetadataFile(metadataFile.string());
            }
        }

        if (mongoRestoreGlobalParams.restoreOptions && metadataObject.hasField("options")) {
            // Try to create collection with given options
            createCollectionWithOptions(metadataObject["options"].Obj());
        }

        // 3) Actually restore the BSONObjs inside the dump file
        processFile( root );

        // 4) If running with --drop, remove any users/roles that were in the system at the
        // beginning of the restore but weren't found in the dump file
        if (mongoRestoreGlobalParams.drop && _curcoll == "system.users") {
            // Delete any users that used to exist but weren't in the dump file
            for (set<UserName>::iterator it = _users.begin(); it != _users.end(); ++it) {
                const UserName& name = *it;
                string dbFieldName = _userDBFieldName;
                if (_curdb != "admin") {
                    // Always use userSource when restoring to the legacy system.users collections
                    // found in non-admin databases, even if the system is otherwise upgrade to v3.
                    dbFieldName = "userSource";
                }

                BSONObjBuilder queryBuilder;
                queryBuilder << "user" << name.getUser();
                if (dbFieldName == "userSource" && name.getDB() == _curdb) {
                    // userSource field won't be present for v1 users docs in the same db as the
                    // user is defined on.
                    queryBuilder << "userSource" << BSONNULL;
                } else {
                    queryBuilder << dbFieldName << name.getDB();
                }
                conn().remove(_curns, Query(queryBuilder.done()));
            }
            _users.clear();
        }
        if (mongoRestoreGlobalParams.drop && _curns == "admin.system.roles") {
            // Delete any roles that used to exist but weren't in the dump file
            for (set<RoleName>::iterator it = _roles.begin(); it != _roles.end(); ++it) {
                const RoleName& name = *it;
                conn().remove(ns, Query(BSON("role" << name.getRole() << "db" << name.getDB())));
            }
            _roles.clear();
        }

        // 5) Restore indexes
        if (mongoRestoreGlobalParams.restoreIndexes && metadataObject.hasField("indexes")) {
            vector<BSONElement> indexes = metadataObject["indexes"].Array();
            for (vector<BSONElement>::iterator it = indexes.begin(); it != indexes.end(); ++it) {
                createIndex((*it).Obj(), false);
            }
        }
    }

    virtual void gotObject( const BSONObj& obj ) {
        if (_curns == OPLOG_SENTINEL) { // intentional ptr compare
            if (obj["op"].valuestr()[0] == 'n') // skip no-ops
                return;
            
            // exclude operations that don't meet (timestamp) criteria
            if ( _opmatcher.get() && ! _opmatcher->matches ( obj ) ) {
                _oplogEntrySkips++;
                return;
            }

            string db = obj["ns"].valuestr();
            db = db.substr(0, db.find('.'));

            BSONObj cmd = BSON( "applyOps" << BSON_ARRAY( obj ) );
            BSONObj out;
            conn().runCommand(db, cmd, out);
            _oplogEntryApplies++;

            // wait for ops to propagate to "w" nodes (doesn't warn if w used without replset)
            if (mongoRestoreGlobalParams.w > 0) {
                string err = conn().getLastError(db, false, false, mongoRestoreGlobalParams.w);
                if (!err.empty()) {
                    toolError() << "Error while replaying oplog: " << err << std::endl;
                }
            }
            return;
        }

        if (nsToCollectionSubstring(_curns) == "system.indexes") {
            createIndex(obj, true);
        }
        else if (mongoRestoreGlobalParams.drop &&
                 _curns == "admin.system.roles" &&
                 _roles.count(RoleName(obj["role"].String(), obj["db"].String()))) {
            // Since system collections can't be dropped, we have to manually
            // replace the contents of the system.roles collection
            BSONObj roleMatch = BSON("role" << obj["role"].String() << "db" << obj["db"].String());
            conn().update(_curns, Query(roleMatch), obj);
            _roles.erase(RoleName(obj["role"].String(), obj["db"].String()));
        }
        else if (_curcoll == "system.users") {
            string userDB;
            uassertStatusOK(bsonExtractStringFieldWithDefault(obj,
                                                              _userDBFieldName,
                                                              _curdb,
                                                              &userDB));

            if (_curdb == "admin" && obj.hasField("credentials")) { // Treat non-admin db like 2.4
                if (_serverAuthzVersion == AuthorizationManager::schemaVersion24) {
                    // v3 user, v1 system
                    uasserted(17407,
                              mongoutils::str::stream()
                                      << "Server has authorization schema version "
                                      << AuthorizationManager::schemaVersion24
                                      << ", but found a schema version "
                                      << AuthorizationManager::schemaVersion26Final << " user: "
                                      << obj.toString());
                } else {
                    // v3 user, v3 system
                    if (mongoRestoreGlobalParams.drop && _users.count(UserName(obj["user"].String(),
                                                                               userDB))) {
                        // Since system collections can't be dropped, we have to manually
                        // replace the contents of the system.users collection
                        BSONObj userMatch = BSON("user" << obj["user"].String() << "db" << userDB);
                        conn().update(_curns, Query(userMatch), obj);
                        _users.erase(UserName(obj["user"].String(), userDB));
                    } else {
                        conn().insert(_curns, obj);
                    }
                }
            } else {
                if (_serverAuthzVersion == AuthorizationManager::schemaVersion24 ||
                        _curdb != "admin" || // Restoring 2.4 schema users to non-admin dbs is OK
                        !_serverAuthzVersionDocExists) { // Clean 2.6 system can have 2.4 users
                    // v1 user, v1 system
                    if (mongoRestoreGlobalParams.drop && _users.count(UserName(obj["user"].String(),
                                                                               userDB))) {
                        // Since system collections can't be dropped, we have to manually
                        // replace the contents of the system.users collection
                        BSONObj userMatch = BSON("user" << obj["user"].String() <<
                                                 "userSource" << userDB);
                        conn().update(_curns, Query(userMatch), obj);
                        _users.erase(UserName(obj["user"].String(), userDB));
                    } else {
                        conn().insert(_curns, obj);
                    }
                } else {
                    // v1 user, v3 system
                    // TODO(spencer): SERVER-12491 Rather than failing here, we should convert the
                    // v1 user to an equivalent v3 schema user
                    uasserted(17408,
                              mongoutils::str::stream()
                                      << "Server has authorization schema version "
                                      << AuthorizationManager::schemaVersion26Final
                                      << ", but found a schema version "
                                      << AuthorizationManager::schemaVersion24 << " user: "
                                      << obj.toString());
                }
            }
        }
        else {
            if (_curns == "admin.system.version") {
                long long authVersion;
                uassertStatusOK(bsonExtractIntegerField(obj,
                                                        AuthorizationManager::schemaVersionFieldName,
                                                        &authVersion));
                _dumpFileAuthzVersion = static_cast<int>(authVersion);
            }
            conn().insert(_curns, obj);
        }

        // wait for insert (or update) to propagate to "w" nodes (doesn't warn if w used
        // without replset)
        if (mongoRestoreGlobalParams.w > 0) {
            string err = conn().getLastError(_curdb, false, false, mongoRestoreGlobalParams.w);
            if (!err.empty()) {
                toolError() << err << std::endl;
            }
        }
    }

private:

    BSONObj parseMetadataFile(string filePath) {
        long long fileSize = boost::filesystem::file_size(filePath);
        ifstream file(filePath.c_str(), ios_base::in);

        boost::scoped_array<char> buf(new char[fileSize]);
        file.read(buf.get(), fileSize);
        int objSize;
        BSONObj obj;
        obj = fromjson (buf.get(), &objSize);
        return obj;
    }

    // Compares 2 BSONObj representing collection options. Returns true if the objects
    // represent different options. Ignores the "create" field.
    bool optionsSame(BSONObj obj1, BSONObj obj2) {
        int nfields = 0;
        BSONObjIterator i(obj1);
        while ( i.more() ) {
            BSONElement e = i.next();
            if (!obj2.hasField(e.fieldName())) {
                if (strcmp(e.fieldName(), "create") == 0) {
                    continue;
                } else {
                    return false;
                }
            }
            nfields++;
            if (e != obj2[e.fieldName()]) {
                return false;
            }
        }
        return nfields == obj2.nFields();
    }

    void createCollectionWithOptions(BSONObj obj) {
        BSONObjIterator i(obj);

        // Rebuild obj as a command object for the "create" command.
        // - {create: <name>} comes first, where <name> is the new name for the collection
        // - elements with type Undefined get skipped over
        BSONObjBuilder bo;
        bo.append("create", _curcoll);
        while (i.more()) {
            BSONElement e = i.next();

            if (strcmp(e.fieldName(), "create") == 0) {
                continue;
            }

            if (e.type() == Undefined) {
                toolInfoLog() << _curns << ": skipping undefined field: " << e.fieldName()
                              << std::endl;
                continue;
            }

            bo.append(e);
        }
        obj = bo.obj();

        BSONObj fields = BSON("options" << 1);
        scoped_ptr<DBClientCursor> cursor(conn().query(_curdb + ".system.namespaces", Query(BSON("name" << _curns)), 0, 0, &fields));

        bool createColl = true;
        if (cursor->more()) {
            createColl = false;
            BSONObj nsObj = cursor->next();
            if (!nsObj.hasField("options") || !optionsSame(obj, nsObj["options"].Obj())) {
                toolError() << "WARNING: collection " << _curns
                          << " exists with different options than are in the metadata.json file and"
                          << " not using --drop. Options in the metadata file will be ignored."
                          << std::endl;
            }
        }

        if (!createColl) {
            return;
        }

        BSONObj info;
        if (!conn().runCommand(_curdb, obj, info)) {
            uasserted(15936, "Creating collection " + _curns + " failed. Errmsg: " + info["errmsg"].String());
        } else {
            toolInfoLog() << "\tCreated collection " << _curns << " with options: "
                          << obj.jsonString() << std::endl;
        }
    }

    /* We must handle if the dbname or collection name is different at restore time than what was dumped.
       If keepCollName is true, however, we keep the same collection name that's in the index object.
     */
    void createIndex(BSONObj indexObj, bool keepCollName) {
        BSONObjBuilder bo;
        BSONObjIterator i(indexObj);
        while ( i.more() ) {
            BSONElement e = i.next();
            if (strcmp(e.fieldName(), "ns") == 0) {
                NamespaceString n(e.String());
                string s = _curdb + "." + (keepCollName ? n.coll().toString() : _curcoll);
                bo.append("ns", s);
            }
            // Remove index version number
            else if (strcmp(e.fieldName(), "v") != 0 || mongoRestoreGlobalParams.keepIndexVersion) {
                bo.append(e);
            }
        }
        BSONObj o = bo.obj();
        if (logger::globalLogDomain()->shouldLog(logger::LogSeverity::Debug(0))) {
            toolInfoLog() << "\tCreating index: " << o << std::endl;
        }
        conn().insert( _curdb + ".system.indexes" ,  o );

        // We're stricter about errors for indexes than for regular data
        BSONObj err = conn().getLastErrorDetailed(_curdb, false, false, mongoRestoreGlobalParams.w);

        if (err.hasField("err") && !err["err"].isNull()) {
            if (err["err"].str() == "norepl" && mongoRestoreGlobalParams.w > 1) {
                toolError() << "Cannot specify write concern for non-replicas" << std::endl;
            }
            else {
                string errCode;

                if (err.hasField("code")) {
                    errCode = str::stream() << err["code"].numberInt();
                }

                toolError() << "Error creating index " << o["ns"].String() << ": "
                          << errCode << " " << err["err"] << std::endl;
            }

            ::abort();
        }

        massert(16441, str::stream() << "Error calling getLastError: " << err["errmsg"],
                err["ok"].trueValue());
    }
};

REGISTER_MONGO_TOOL(Restore);
