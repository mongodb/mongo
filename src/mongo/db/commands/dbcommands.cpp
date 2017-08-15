/**
 *    Copyright (C) 2012-2015 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include <array>
#include <time.h>

#include "mongo/base/disallow_copying.h"
#include "mongo/base/simple_string_data_comparator.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/audit.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/user_management_commands_parser.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/background.h"
#include "mongo/db/catalog/coll_mod.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_catalog_entry.h"
#include "mongo/db/catalog/create_collection.h"
#include "mongo/db/catalog/drop_collection.h"
#include "mongo/db/catalog/drop_database.h"
#include "mongo/db/catalog/index_key_validate.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/server_status.h"
#include "mongo/db/commands/shutdown.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/diag_log.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/index_builder.h"
#include "mongo/db/introspect.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/json.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/lasterror.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/op_observer.h"
#include "mongo/db/ops/insert.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/read_concern.h"
#include "mongo/db/repair_database.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/server_parameters.h"
#include "mongo/db/stats/storage_stats.h"
#include "mongo/db/write_concern.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/grid.h"
#include "mongo/s/stale_exception.h"
#include "mongo/scripting/engine.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"
#include "mongo/util/md5.hpp"
#include "mongo/util/scopeguard.h"

namespace mongo {

using std::ostringstream;
using std::string;
using std::stringstream;
using std::unique_ptr;


class CmdShutdownMongoD : public CmdShutdown {
public:
    virtual void help(stringstream& help) const {
        help << "shutdown the database.  must be ran against admin db and "
             << "either (1) ran from localhost or (2) authenticated. If "
             << "this is a primary in a replica set and there is no member "
             << "within 10 seconds of its optime, it will not shutdown "
             << "without force : true.  You can also specify timeoutSecs : "
             << "N to wait N seconds for other members to catch up.";
    }

    virtual bool run(OperationContext* opCtx,
                     const string& dbname,
                     const BSONObj& cmdObj,
                     BSONObjBuilder& result) {
        bool force = cmdObj.hasField("force") && cmdObj["force"].trueValue();

        long long timeoutSecs = 10;
        if (cmdObj.hasField("timeoutSecs")) {
            timeoutSecs = cmdObj["timeoutSecs"].numberLong();
        }

        Status status = repl::getGlobalReplicationCoordinator()->stepDown(
            opCtx, force, Seconds(timeoutSecs), Seconds(120));
        if (!status.isOK() && status.code() != ErrorCodes::NotMaster) {  // ignore not master
            return appendCommandStatus(result, status);
        }

        // Never returns
        shutdownHelper();
        return true;
    }

} cmdShutdownMongoD;

class CmdDropDatabase : public BasicCommand {
public:
    virtual void help(stringstream& help) const {
        help << "drop (delete) this database";
    }
    virtual bool slaveOk() const {
        return false;
    }

    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) {
        ActionSet actions;
        actions.addAction(ActionType::dropDatabase);
        out->push_back(Privilege(ResourcePattern::forDatabaseName(dbname), actions));
    }


    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    CmdDropDatabase() : BasicCommand("dropDatabase") {}

    bool run(OperationContext* opCtx,
             const string& dbname,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) {
        // disallow dropping the config database
        if (serverGlobalParams.clusterRole == ClusterRole::ConfigServer &&
            (dbname == NamespaceString::kConfigDb)) {
            return appendCommandStatus(result,
                                       Status(ErrorCodes::IllegalOperation,
                                              "Cannot drop 'config' database if mongod started "
                                              "with --configsvr"));
        }

        if ((repl::getGlobalReplicationCoordinator()->getReplicationMode() !=
             repl::ReplicationCoordinator::modeNone) &&
            (dbname == NamespaceString::kLocalDb)) {
            return appendCommandStatus(result,
                                       Status(ErrorCodes::IllegalOperation,
                                              "Cannot drop 'local' database while replication "
                                              "is active"));
        }
        BSONElement e = cmdObj.firstElement();
        int p = (int)e.number();
        if (p != 1) {
            return appendCommandStatus(
                result, Status(ErrorCodes::IllegalOperation, "have to pass 1 as db parameter"));
        }

        Status status = dropDatabase(opCtx, dbname);
        if (status == ErrorCodes::NamespaceNotFound) {
            return appendCommandStatus(result, Status::OK());
        }
        if (status.isOK()) {
            result.append("dropped", dbname);
        }
        return appendCommandStatus(result, status);
    }

} cmdDropDatabase;

class CmdRepairDatabase : public ErrmsgCommandDeprecated {
public:
    virtual bool slaveOk() const {
        return true;
    }
    virtual bool maintenanceMode() const {
        return true;
    }
    virtual void help(stringstream& help) const {
        help << "repair database.  also compacts. note: slow.";
    }


    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) {
        ActionSet actions;
        actions.addAction(ActionType::repairDatabase);
        out->push_back(Privilege(ResourcePattern::forDatabaseName(dbname), actions));
    }

    CmdRepairDatabase() : ErrmsgCommandDeprecated("repairDatabase") {}

    bool errmsgRun(OperationContext* opCtx,
                   const string& dbname,
                   const BSONObj& cmdObj,
                   string& errmsg,
                   BSONObjBuilder& result) {
        BSONElement e = cmdObj.firstElement();
        if (e.numberInt() != 1) {
            errmsg = "bad option";
            return false;
        }

        // Closing a database requires a global lock.
        Lock::GlobalWrite lk(opCtx);
        if (!dbHolder().get(opCtx, dbname)) {
            // If the name doesn't make an exact match, check for a case insensitive match.
            std::set<std::string> otherCasing = dbHolder().getNamesWithConflictingCasing(dbname);
            if (otherCasing.empty()) {
                // Database doesn't exist. Treat this as a success (historical behavior).
                return true;
            }

            // Database exists with a differing case. Treat this as an error. Report the casing
            // conflict.
            errmsg = str::stream() << "Database exists with a different case. Given: `" << dbname
                                   << "` Found: `" << *otherCasing.begin() << "`";
            return false;
        }

        // TODO (Kal): OldClientContext legacy, needs to be removed
        {
            CurOp::get(opCtx)->ensureStarted();
            stdx::lock_guard<Client> lk(*opCtx->getClient());
            CurOp::get(opCtx)->setNS_inlock(dbname);
        }

        log() << "repairDatabase " << dbname;
        BackgroundOperation::assertNoBgOpInProgForDb(dbname);

        e = cmdObj.getField("preserveClonedFilesOnFailure");
        bool preserveClonedFilesOnFailure = e.isBoolean() && e.boolean();
        e = cmdObj.getField("backupOriginalFiles");
        bool backupOriginalFiles = e.isBoolean() && e.boolean();

        StorageEngine* engine = getGlobalServiceContext()->getGlobalStorageEngine();
        repl::UnreplicatedWritesBlock uwb(opCtx);
        Status status = repairDatabase(
            opCtx, engine, dbname, preserveClonedFilesOnFailure, backupOriginalFiles);

        // Open database before returning
        dbHolder().openDb(opCtx, dbname);
        return appendCommandStatus(result, status);
    }
} cmdRepairDatabase;

/* set db profiling level
   todo: how do we handle profiling information put in the db with replication?
         sensibly or not?
*/
class CmdProfile : public ErrmsgCommandDeprecated {
public:
    virtual bool slaveOk() const {
        return true;
    }

    virtual void help(stringstream& help) const {
        help << "enable or disable performance profiling\n";
        help << "{ profile : <n> }\n";
        help << "0=off 1=log slow ops 2=log all\n";
        help << "-1 to get current values\n";
        help << "http://docs.mongodb.org/manual/reference/command/profile/#dbcmd.profile";
    }


    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    virtual Status checkAuthForCommand(Client* client,
                                       const std::string& dbname,
                                       const BSONObj& cmdObj) {
        AuthorizationSession* authzSession = AuthorizationSession::get(client);

        if (cmdObj.firstElement().numberInt() == -1 && !cmdObj.hasField("slowms") &&
            !cmdObj.hasField("sampleRate")) {
            // If you just want to get the current profiling level you can do so with just
            // read access to system.profile, even if you can't change the profiling level.
            if (authzSession->isAuthorizedForActionsOnResource(
                    ResourcePattern::forExactNamespace(NamespaceString(dbname, "system.profile")),
                    ActionType::find)) {
                return Status::OK();
            }
        }

        if (authzSession->isAuthorizedForActionsOnResource(ResourcePattern::forDatabaseName(dbname),
                                                           ActionType::enableProfiler)) {
            return Status::OK();
        }

        return Status(ErrorCodes::Unauthorized, "unauthorized");
    }

    CmdProfile() : ErrmsgCommandDeprecated("profile") {}

    bool errmsgRun(OperationContext* opCtx,
                   const string& dbname,
                   const BSONObj& cmdObj,
                   string& errmsg,
                   BSONObjBuilder& result) {
        BSONElement firstElement = cmdObj.firstElement();
        int profilingLevel = firstElement.numberInt();

        // If profilingLevel is 0, 1, or 2, needs to be locked exclusively,
        // because creates the system.profile collection in the local database.

        const bool readOnly = (profilingLevel < 0 || profilingLevel > 2);
        const LockMode dbMode = readOnly ? MODE_S : MODE_X;

        Status status = Status::OK();

        AutoGetDb ctx(opCtx, dbname, dbMode);
        Database* db = ctx.getDb();

        result.append("was", db ? db->getProfilingLevel() : serverGlobalParams.defaultProfile);
        result.append("slowms", serverGlobalParams.slowMS);
        result.append("sampleRate", serverGlobalParams.sampleRate);

        if (!readOnly) {
            if (!db) {
                // When setting the profiling level, create the database if it didn't already exist.
                // When just reading the profiling level, we do not create the database.
                db = dbHolder().openDb(opCtx, dbname);
            }
            status = db->setProfilingLevel(opCtx, profilingLevel);
        }

        const BSONElement slow = cmdObj["slowms"];
        if (slow.isNumber()) {
            serverGlobalParams.slowMS = slow.numberInt();
        }

        double newSampleRate;
        uassertStatusOK(bsonExtractDoubleFieldWithDefault(
            cmdObj, "sampleRate"_sd, serverGlobalParams.sampleRate, &newSampleRate));
        uassert(ErrorCodes::BadValue,
                "sampleRate must be between 0.0 and 1.0 inclusive",
                newSampleRate >= 0.0 && newSampleRate <= 1.0);
        serverGlobalParams.sampleRate = newSampleRate;

        if (!status.isOK()) {
            errmsg = status.reason();
        }

        return status.isOK();
    }

} cmdProfile;

class CmdDiagLogging : public BasicCommand {
public:
    virtual bool slaveOk() const {
        return true;
    }
    CmdDiagLogging() : BasicCommand("diagLogging") {}
    bool adminOnly() const {
        return true;
    }

    void help(stringstream& h) const {
        h << "http://dochub.mongodb.org/core/"
             "monitoring#MonitoringandDiagnostics-DatabaseRecord%2FReplay%28diagLoggingcommand%29";
    }


    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) {
        ActionSet actions;
        actions.addAction(ActionType::diagLogging);
        out->push_back(Privilege(ResourcePattern::forClusterResource(), actions));
    }

    bool run(OperationContext* opCtx,
             const string& dbname,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) {
        const char* deprecationWarning =
            "CMD diagLogging is deprecated and will be removed in a future release";
        warning() << deprecationWarning << startupWarningsLog;

        // This doesn't look like it requires exclusive DB lock, because it uses its own diag
        // locking, but originally the lock was set to be WRITE, so preserving the behaviour.
        Lock::DBLock dbXLock(opCtx, dbname, MODE_X);

        // TODO (Kal): OldClientContext legacy, needs to be removed
        {
            CurOp::get(opCtx)->ensureStarted();
            stdx::lock_guard<Client> lk(*opCtx->getClient());
            CurOp::get(opCtx)->setNS_inlock(dbname);
        }

        int was = _diaglog.setLevel(cmdObj.firstElement().numberInt());
        _diaglog.flush();
        if (!serverGlobalParams.quiet.load()) {
            LOG(0) << "CMD: diagLogging set to " << _diaglog.getLevel() << " from: " << was;
        }
        result.append("was", was);
        result.append("note", deprecationWarning);
        return true;
    }
} cmddiaglogging;

/* drop collection */
class CmdDrop : public ErrmsgCommandDeprecated {
public:
    CmdDrop() : ErrmsgCommandDeprecated("drop") {}
    virtual bool slaveOk() const {
        return false;
    }
    virtual bool adminOnly() const {
        return false;
    }
    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) {
        ActionSet actions;
        actions.addAction(ActionType::dropCollection);
        out->push_back(Privilege(parseResourcePattern(dbname, cmdObj), actions));
    }
    virtual void help(stringstream& help) const {
        help << "drop a collection\n{drop : <collectionName>}";
    }


    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    virtual bool errmsgRun(OperationContext* opCtx,
                           const string& dbname,
                           const BSONObj& cmdObj,
                           string& errmsg,
                           BSONObjBuilder& result) {
        const NamespaceString nsToDrop = parseNsCollectionRequired(dbname, cmdObj);

        if (NamespaceString::virtualized(nsToDrop.ns())) {
            errmsg = "can't drop a virtual collection";
            return false;
        }

        if ((repl::getGlobalReplicationCoordinator()->getReplicationMode() !=
             repl::ReplicationCoordinator::modeNone) &&
            nsToDrop.isOplog()) {
            errmsg = "can't drop live oplog while replicating";
            return false;
        }

        return appendCommandStatus(
            result,
            dropCollection(opCtx,
                           nsToDrop,
                           result,
                           {},
                           DropCollectionSystemCollectionMode::kDisallowSystemCollectionDrops));
    }

} cmdDrop;

/* create collection */
class CmdCreate : public BasicCommand {
public:
    CmdCreate() : BasicCommand("create") {}
    virtual bool slaveOk() const {
        return false;
    }
    virtual bool adminOnly() const {
        return false;
    }


    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    virtual void help(stringstream& help) const {
        help << "create a collection explicitly\n"
                "{ create: <ns>[, capped: <bool>, size: <collSizeInBytes>, max: <nDocs>] }";
    }
    virtual Status checkAuthForCommand(Client* client,
                                       const std::string& dbname,
                                       const BSONObj& cmdObj) {
        const NamespaceString nss(parseNs(dbname, cmdObj));
        return AuthorizationSession::get(client)->checkAuthForCreate(nss, cmdObj, false);
    }

    virtual bool run(OperationContext* opCtx,
                     const string& dbname,
                     const BSONObj& cmdObj,
                     BSONObjBuilder& result) {
        const NamespaceString ns(parseNsCollectionRequired(dbname, cmdObj));

        if (cmdObj.hasField("autoIndexId")) {
            const char* deprecationWarning =
                "the autoIndexId option is deprecated and will be removed in a future release";
            warning() << deprecationWarning;
            result.append("note", deprecationWarning);
        }

        // Validate _id index spec and fill in missing fields.
        if (auto idIndexElem = cmdObj["idIndex"]) {
            if (cmdObj["viewOn"]) {
                return appendCommandStatus(
                    result,
                    {ErrorCodes::InvalidOptions,
                     str::stream() << "'idIndex' is not allowed with 'viewOn': " << idIndexElem});
            }
            if (cmdObj["autoIndexId"]) {
                return appendCommandStatus(result,
                                           {ErrorCodes::InvalidOptions,
                                            str::stream()
                                                << "'idIndex' is not allowed with 'autoIndexId': "
                                                << idIndexElem});
            }

            if (idIndexElem.type() != BSONType::Object) {
                return appendCommandStatus(
                    result,
                    {ErrorCodes::TypeMismatch,
                     str::stream() << "'idIndex' has to be a document: " << idIndexElem});
            }

            auto idIndexSpec = idIndexElem.Obj();

            // Perform index spec validation.
            idIndexSpec = uassertStatusOK(index_key_validate::validateIndexSpec(
                idIndexSpec, ns, serverGlobalParams.featureCompatibility));
            uassertStatusOK(index_key_validate::validateIdIndexSpec(idIndexSpec));

            // Validate or fill in _id index collation.
            std::unique_ptr<CollatorInterface> defaultCollator;
            if (auto collationElem = cmdObj["collation"]) {
                if (collationElem.type() != BSONType::Object) {
                    return appendCommandStatus(
                        result,
                        {ErrorCodes::TypeMismatch,
                         str::stream() << "'collation' has to be a document: " << collationElem});
                }
                auto collatorStatus = CollatorFactoryInterface::get(opCtx->getServiceContext())
                                          ->makeFromBSON(collationElem.Obj());
                if (!collatorStatus.isOK()) {
                    return appendCommandStatus(result, collatorStatus.getStatus());
                }
                defaultCollator = std::move(collatorStatus.getValue());
            }
            idIndexSpec = uassertStatusOK(index_key_validate::validateIndexSpecCollation(
                opCtx, idIndexSpec, defaultCollator.get()));
            std::unique_ptr<CollatorInterface> idIndexCollator;
            if (auto collationElem = idIndexSpec["collation"]) {
                auto collatorStatus = CollatorFactoryInterface::get(opCtx->getServiceContext())
                                          ->makeFromBSON(collationElem.Obj());
                // validateIndexSpecCollation() should have checked that the _id index collation
                // spec is valid.
                invariant(collatorStatus.isOK());
                idIndexCollator = std::move(collatorStatus.getValue());
            }
            if (!CollatorInterface::collatorsMatch(defaultCollator.get(), idIndexCollator.get())) {
                return appendCommandStatus(
                    result,
                    {ErrorCodes::BadValue,
                     "'idIndex' must have the same collation as the collection."});
            }

            // Remove "idIndex" field from command.
            auto resolvedCmdObj = cmdObj.removeField("idIndex");

            return appendCommandStatus(
                result, createCollection(opCtx, dbname, resolvedCmdObj, idIndexSpec));
        }

        BSONObj idIndexSpec;
        return appendCommandStatus(result, createCollection(opCtx, dbname, cmdObj, idIndexSpec));
    }
} cmdCreate;


class CmdFileMD5 : public BasicCommand {
public:
    CmdFileMD5() : BasicCommand("filemd5") {}

    virtual bool slaveOk() const {
        return true;
    }

    virtual void help(stringstream& help) const {
        help << " example: { filemd5 : ObjectId(aaaaaaa) , root : \"fs\" }";
    }


    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    virtual std::string parseNs(const std::string& dbname, const BSONObj& cmdObj) const {
        std::string collectionName;
        if (const auto rootElt = cmdObj["root"]) {
            uassert(ErrorCodes::InvalidNamespace,
                    "'root' must be of type String",
                    rootElt.type() == BSONType::String);
            collectionName = rootElt.str();
        }
        if (collectionName.empty())
            collectionName = "fs";
        collectionName += ".chunks";
        return NamespaceString(dbname, collectionName).ns();
    }

    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) {
        out->push_back(Privilege(parseResourcePattern(dbname, cmdObj), ActionType::find));
    }

    bool run(OperationContext* opCtx,
             const string& dbname,
             const BSONObj& jsobj,
             BSONObjBuilder& result) {
        const NamespaceString nss(parseNs(dbname, jsobj));

        md5digest d;
        md5_state_t st;
        md5_init(&st);

        int n = 0;

        bool partialOk = jsobj["partialOk"].trueValue();
        if (partialOk) {
            // WARNING: This code depends on the binary layout of md5_state. It will not be
            // compatible with different md5 libraries or work correctly in an environment with
            // mongod's of different endians. It is ok for mongos to be a different endian since
            // it just passes the buffer through to another mongod.
            BSONElement stateElem = jsobj["md5state"];
            if (!stateElem.eoo()) {
                int len;
                const char* data = stateElem.binDataClean(len);
                massert(16247, "md5 state not correct size", len == sizeof(st));
                memcpy(&st, data, sizeof(st));
            }
            n = jsobj["startAt"].numberInt();
        }

        BSONObj query = BSON("files_id" << jsobj["filemd5"] << "n" << GTE << n);
        BSONObj sort = BSON("files_id" << 1 << "n" << 1);

        return writeConflictRetry(opCtx, "filemd5", dbname, [&] {
            auto qr = stdx::make_unique<QueryRequest>(nss);
            qr->setFilter(query);
            qr->setSort(sort);

            auto statusWithCQ = CanonicalQuery::canonicalize(opCtx, std::move(qr));
            if (!statusWithCQ.isOK()) {
                uasserted(17240, "Can't canonicalize query " + query.toString());
                return false;
            }
            unique_ptr<CanonicalQuery> cq = std::move(statusWithCQ.getValue());

            // Check shard version at startup.
            // This will throw before we've done any work if shard version is outdated
            // We drop and re-acquire these locks every document because md5'ing is expensive
            unique_ptr<AutoGetCollectionForReadCommand> ctx(
                new AutoGetCollectionForReadCommand(opCtx, nss));
            Collection* coll = ctx->getCollection();

            auto exec = uassertStatusOK(getExecutor(opCtx,
                                                    coll,
                                                    std::move(cq),
                                                    PlanExecutor::YIELD_MANUAL,
                                                    QueryPlannerParams::NO_TABLE_SCAN));

            BSONObj obj;
            PlanExecutor::ExecState state;
            while (PlanExecutor::ADVANCED == (state = exec->getNext(&obj, NULL))) {
                BSONElement ne = obj["n"];
                verify(ne.isNumber());
                int myn = ne.numberInt();
                if (n != myn) {
                    if (partialOk) {
                        break;  // skipped chunk is probably on another shard
                    }
                    log() << "should have chunk: " << n << " have:" << myn;
                    dumpChunks(opCtx, nss.ns(), query, sort);
                    uassert(10040, "chunks out of order", n == myn);
                }

                // make a copy of obj since we access data in it while yielding locks
                BSONObj owned = obj.getOwned();
                exec->saveState();
                // UNLOCKED
                ctx.reset();

                int len;
                const char* data = owned["data"].binDataClean(len);
                // This is potentially an expensive operation, so do it out of the lock
                md5_append(&st, (const md5_byte_t*)(data), len);
                n++;

                try {
                    // RELOCKED
                    ctx.reset(new AutoGetCollectionForReadCommand(opCtx, nss));
                } catch (const SendStaleConfigException& ex) {
                    LOG(1) << "chunk metadata changed during filemd5, will retarget and continue";
                    break;
                }

                // Have the lock again. See if we were killed.
                if (!exec->restoreState().isOK()) {
                    if (!partialOk) {
                        uasserted(13281, "File deleted during filemd5 command");
                    }
                }
            }

            if (PlanExecutor::DEAD == state || PlanExecutor::FAILURE == state) {
                return appendCommandStatus(result,
                                           Status(ErrorCodes::OperationFailed,
                                                  str::stream()
                                                      << "Executor error during filemd5 command: "
                                                      << WorkingSetCommon::toStatusString(obj)));
            }

            if (partialOk)
                result.appendBinData("md5state", sizeof(st), BinDataGeneral, &st);

            // This must be *after* the capture of md5state since it mutates st
            md5_finish(&st, d);

            result.append("numChunks", n);
            result.append("md5", digestToString(d));

            return true;
        });
    }

    void dumpChunks(OperationContext* opCtx,
                    const string& ns,
                    const BSONObj& query,
                    const BSONObj& sort) {
        DBDirectClient client(opCtx);
        Query q(query);
        q.sort(sort);
        unique_ptr<DBClientCursor> c = client.query(ns, q);
        while (c->more()) {
            log() << c->nextSafe();
        }
    }

} cmdFileMD5;


class CmdDatasize : public ErrmsgCommandDeprecated {
    virtual string parseNs(const string& dbname, const BSONObj& cmdObj) const {
        return parseNsFullyQualified(dbname, cmdObj);
    }

public:
    CmdDatasize() : ErrmsgCommandDeprecated("dataSize", "datasize") {}

    virtual bool slaveOk() const {
        return true;
    }
    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }
    virtual void help(stringstream& help) const {
        help << "determine data size for a set of data in a certain range"
                "\nexample: { dataSize:\"blog.posts\", keyPattern:{x:1}, min:{x:10}, max:{x:55} }"
                "\nmin and max parameters are optional. They must either both be included or both "
                "omitted"
                "\nkeyPattern is an optional parameter indicating an index pattern that would be "
                "useful"
                "for iterating over the min/max bounds. If keyPattern is omitted, it is inferred "
                "from "
                "the structure of min. "
                "\nnote: This command may take a while to run";
    }

    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) {
        ActionSet actions;
        actions.addAction(ActionType::find);
        out->push_back(Privilege(parseResourcePattern(dbname, cmdObj), actions));
    }

    bool errmsgRun(OperationContext* opCtx,
                   const string& dbname,
                   const BSONObj& jsobj,
                   string& errmsg,
                   BSONObjBuilder& result) {
        Timer timer;

        string ns = jsobj.firstElement().String();
        BSONObj min = jsobj.getObjectField("min");
        BSONObj max = jsobj.getObjectField("max");
        BSONObj keyPattern = jsobj.getObjectField("keyPattern");
        bool estimate = jsobj["estimate"].trueValue();

        AutoGetCollectionForReadCommand ctx(opCtx, NamespaceString(ns));

        Collection* collection = ctx.getCollection();
        long long numRecords = 0;
        if (collection) {
            numRecords = collection->numRecords(opCtx);
        }

        if (numRecords == 0) {
            result.appendNumber("size", 0);
            result.appendNumber("numObjects", 0);
            result.append("millis", timer.millis());
            return true;
        }

        result.appendBool("estimate", estimate);

        unique_ptr<PlanExecutor, PlanExecutor::Deleter> exec;
        if (min.isEmpty() && max.isEmpty()) {
            if (estimate) {
                result.appendNumber("size", static_cast<long long>(collection->dataSize(opCtx)));
                result.appendNumber("numObjects", numRecords);
                result.append("millis", timer.millis());
                return 1;
            }
            exec = InternalPlanner::collectionScan(opCtx, ns, collection, PlanExecutor::NO_YIELD);
        } else if (min.isEmpty() || max.isEmpty()) {
            errmsg = "only one of min or max specified";
            return false;
        } else {
            if (keyPattern.isEmpty()) {
                // if keyPattern not provided, try to infer it from the fields in 'min'
                keyPattern = Helpers::inferKeyPattern(min);
            }

            IndexDescriptor* idx =
                collection->getIndexCatalog()->findShardKeyPrefixedIndex(opCtx,
                                                                         keyPattern,
                                                                         true);  // requireSingleKey

            if (idx == NULL) {
                errmsg = "couldn't find valid index containing key pattern";
                return false;
            }
            // If both min and max non-empty, append MinKey's to make them fit chosen index
            KeyPattern kp(idx->keyPattern());
            min = Helpers::toKeyFormat(kp.extendRangeBound(min, false));
            max = Helpers::toKeyFormat(kp.extendRangeBound(max, false));

            exec = InternalPlanner::indexScan(opCtx,
                                              collection,
                                              idx,
                                              min,
                                              max,
                                              BoundInclusion::kIncludeStartKeyOnly,
                                              PlanExecutor::NO_YIELD);
        }

        long long avgObjSize = collection->dataSize(opCtx) / numRecords;

        long long maxSize = jsobj["maxSize"].numberLong();
        long long maxObjects = jsobj["maxObjects"].numberLong();

        long long size = 0;
        long long numObjects = 0;

        RecordId loc;
        BSONObj obj;
        PlanExecutor::ExecState state;
        while (PlanExecutor::ADVANCED == (state = exec->getNext(&obj, &loc))) {
            if (estimate)
                size += avgObjSize;
            else
                size += collection->getRecordStore()->dataFor(opCtx, loc).size();

            numObjects++;

            if ((maxSize && size > maxSize) || (maxObjects && numObjects > maxObjects)) {
                result.appendBool("maxReached", true);
                break;
            }
        }

        if (PlanExecutor::FAILURE == state || PlanExecutor::DEAD == state) {
            warning() << "Internal error while reading " << ns;
            return appendCommandStatus(
                result,
                Status(ErrorCodes::OperationFailed,
                       str::stream() << "Executor error while reading during dataSize command: "
                                     << WorkingSetCommon::toStatusString(obj)));
        }

        ostringstream os;
        os << "Finding size for ns: " << ns;
        if (!min.isEmpty()) {
            os << " between " << min << " and " << max;
        }

        result.appendNumber("size", size);
        result.appendNumber("numObjects", numObjects);
        result.append("millis", timer.millis());
        return true;
    }

} cmdDatasize;

class CollectionStats : public ErrmsgCommandDeprecated {
public:
    CollectionStats() : ErrmsgCommandDeprecated("collStats", "collstats") {}

    virtual bool slaveOk() const {
        return true;
    }
    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }
    virtual void help(stringstream& help) const {
        help
            << "{ collStats:\"blog.posts\" , scale : 1 } scale divides sizes e.g. for KB use 1024\n"
               "    avgObjSize - in bytes";
    }

    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) {
        ActionSet actions;
        actions.addAction(ActionType::collStats);
        out->push_back(Privilege(parseResourcePattern(dbname, cmdObj), actions));
    }

    bool errmsgRun(OperationContext* opCtx,
                   const string& dbname,
                   const BSONObj& jsobj,
                   string& errmsg,
                   BSONObjBuilder& result) {
        const NamespaceString nss(parseNsCollectionRequired(dbname, jsobj));

        if (nss.coll().empty()) {
            errmsg = "No collection name specified";
            return false;
        }

        result.append("ns", nss.ns());
        Status status = appendCollectionStorageStats(opCtx, nss, jsobj, &result);
        if (!status.isOK()) {
            errmsg = status.reason();
            return false;
        }

        return true;
    }

} cmdCollectionStats;

class CollectionModCommand : public BasicCommand {
public:
    CollectionModCommand() : BasicCommand("collMod") {}

    virtual bool slaveOk() const {
        return false;
    }
    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }
    virtual void help(stringstream& help) const {
        help << "Sets collection options.\n"
                "Example: { collMod: 'foo', usePowerOf2Sizes:true }\n"
                "Example: { collMod: 'foo', index: {keyPattern: {a: 1}, expireAfterSeconds: 600} "
                "Example: { collMod: 'foo', index: {name: 'bar', expireAfterSeconds: 600} }\n";
    }

    virtual Status checkAuthForCommand(Client* client,
                                       const std::string& dbname,
                                       const BSONObj& cmdObj) {
        const NamespaceString nss(parseNs(dbname, cmdObj));
        return AuthorizationSession::get(client)->checkAuthForCollMod(nss, cmdObj, false);
    }

    bool run(OperationContext* opCtx,
             const string& dbname,
             const BSONObj& jsobj,
             BSONObjBuilder& result) {
        const NamespaceString nss(parseNsCollectionRequired(dbname, jsobj));
        return appendCommandStatus(result, collMod(opCtx, nss, jsobj, &result));
    }

} collectionModCommand;

class DBStats : public ErrmsgCommandDeprecated {
public:
    DBStats() : ErrmsgCommandDeprecated("dbStats", "dbstats") {}

    virtual bool slaveOk() const {
        return true;
    }
    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }
    virtual void help(stringstream& help) const {
        help << "Get stats on a database. Not instantaneous. Slower for databases with large "
                ".ns files.\n"
                "Example: { dbStats:1, scale:1 }";
    }

    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) {
        ActionSet actions;
        actions.addAction(ActionType::dbStats);
        out->push_back(Privilege(ResourcePattern::forDatabaseName(dbname), actions));
    }

    bool errmsgRun(OperationContext* opCtx,
                   const string& dbname,
                   const BSONObj& jsobj,
                   string& errmsg,
                   BSONObjBuilder& result) {
        int scale = 1;
        if (jsobj["scale"].isNumber()) {
            scale = jsobj["scale"].numberInt();
            if (scale <= 0) {
                errmsg = "scale has to be > 0";
                return false;
            }
        } else if (jsobj["scale"].trueValue()) {
            errmsg = "scale has to be a number > 0";
            return false;
        }

        const string ns = parseNs(dbname, jsobj);
        uassert(ErrorCodes::InvalidNamespace,
                str::stream() << "Invalid db name: " << ns,
                NamespaceString::validDBName(ns, NamespaceString::DollarInDbNameBehavior::Allow));

        // TODO (Kal): OldClientContext legacy, needs to be removed
        {
            CurOp::get(opCtx)->ensureStarted();
            stdx::lock_guard<Client> lk(*opCtx->getClient());
            CurOp::get(opCtx)->setNS_inlock(dbname);
        }

        // We lock the entire database in S-mode in order to ensure that the contents will not
        // change for the stats snapshot. This might be unnecessary and if it becomes a
        // performance issue, we can take IS lock and then lock collection-by-collection.
        AutoGetDb autoDb(opCtx, ns, MODE_S);

        result.append("db", ns);

        Database* db = autoDb.getDb();
        if (!db) {
            // TODO: This preserves old behaviour where we used to create an empty database
            // metadata even when the database is accessed for read. Without this several
            // unit-tests will fail, which are fairly easy to fix. If backwards compatibility
            // is not needed for the missing DB case, we can just do the same that's done in
            // CollectionStats.
            result.appendNumber("collections", 0);
            result.appendNumber("views", 0);
            result.appendNumber("objects", 0);
            result.append("avgObjSize", 0);
            result.appendNumber("dataSize", 0);
            result.appendNumber("storageSize", 0);
            result.appendNumber("numExtents", 0);
            result.appendNumber("indexes", 0);
            result.appendNumber("indexSize", 0);
            result.appendNumber("fileSize", 0);
            if (!getGlobalServiceContext()->getGlobalStorageEngine()->isEphemeral()) {
                result.appendNumber("fsUsedSize", 0);
                result.appendNumber("fsTotalSize", 0);
            }
        } else {
            {
                stdx::lock_guard<Client> lk(*opCtx->getClient());
                // TODO: OldClientContext legacy, needs to be removed
                CurOp::get(opCtx)->enter_inlock(dbname.c_str(), db->getProfilingLevel());
            }

            db->getStats(opCtx, &result, scale);
        }

        return true;
    }

} cmdDBStats;

/* Returns client's uri */
class CmdWhatsMyUri : public BasicCommand {
public:
    CmdWhatsMyUri() : BasicCommand("whatsmyuri") {}
    virtual bool slaveOk() const {
        return true;
    }
    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }
    virtual void help(stringstream& help) const {
        help << "{whatsmyuri:1}";
    }
    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) {}  // No auth required
    virtual bool run(OperationContext* opCtx,
                     const string& dbname,
                     const BSONObj& cmdObj,
                     BSONObjBuilder& result) {
        result << "you" << opCtx->getClient()->clientAddress(true /*includePort*/);
        return true;
    }
} cmdWhatsMyUri;

class AvailableQueryOptions : public BasicCommand {
public:
    AvailableQueryOptions() : BasicCommand("availableQueryOptions", "availablequeryoptions") {}

    virtual bool slaveOk() const {
        return true;
    }
    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }
    virtual Status checkAuthForCommand(Client* client,
                                       const std::string& dbname,
                                       const BSONObj& cmdObj) {
        return Status::OK();
    }

    virtual bool run(OperationContext* opCtx,
                     const string& dbname,
                     const BSONObj& cmdObj,
                     BSONObjBuilder& result) {
        result << "options" << QueryOption_AllSupported;
        return true;
    }
} availableQueryOptionsCmd;

}  // namespace mongo
