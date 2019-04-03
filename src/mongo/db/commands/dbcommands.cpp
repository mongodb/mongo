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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include <time.h>

#include "mongo/base/simple_string_data_comparator.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/audit.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/user_management_commands_parser.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/background.h"
#include "mongo/db/catalog/coll_mod.h"
#include "mongo/db/catalog/create_collection.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/drop_collection.h"
#include "mongo/db/catalog/drop_database.h"
#include "mongo/db/catalog/index_key_validate.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/create_gen.h"
#include "mongo/db/commands/profile_common.h"
#include "mongo/db/commands/profile_gen.h"
#include "mongo/db/commands/server_status.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/index/index_descriptor.h"
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
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/stats/storage_stats.h"
#include "mongo/db/storage/storage_engine_init.h"
#include "mongo/db/write_concern.h"
#include "mongo/scripting/engine.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"
#include "mongo/util/md5.hpp"
#include "mongo/util/scopeguard.h"
#include "mongo/util/version.h"

namespace mongo {

using std::ostringstream;
using std::string;
using std::stringstream;
using std::unique_ptr;

namespace {

class CmdDropDatabase : public BasicCommand {
public:
    std::string help() const override {
        return "drop (delete) this database";
    }
    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) const {
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
            uasserted(ErrorCodes::IllegalOperation,
                      "Cannot drop 'config' database if mongod started "
                      "with --configsvr");
        }

        if ((repl::ReplicationCoordinator::get(opCtx)->getReplicationMode() !=
             repl::ReplicationCoordinator::modeNone) &&
            (dbname == NamespaceString::kLocalDb)) {
            uasserted(ErrorCodes::IllegalOperation,
                      str::stream() << "Cannot drop '" << dbname
                                    << "' database while replication is active");
        }
        BSONElement e = cmdObj.firstElement();
        int p = (int)e.number();
        if (p != 1) {
            uasserted(ErrorCodes::IllegalOperation, "have to pass 1 as db parameter");
        }

        Status status = dropDatabase(opCtx, dbname);
        if (status == ErrorCodes::NamespaceNotFound) {
            return true;
        }
        if (status.isOK()) {
            result.append("dropped", dbname);
        }
        uassertStatusOK(status);
        return true;
    }

} cmdDropDatabase;

static const char* repairRemovedMessage =
    "This command has been removed. If you would like to compact your data, use the 'compact' "
    "command. If you would like to rebuild indexes, use the 'reIndex' command. If you need to "
    "recover data, please see the documentation for repairing your database offline: "
    "http://dochub.mongodb.org/core/repair";

class CmdRepairDatabase : public ErrmsgCommandDeprecated {
public:
    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }
    virtual bool maintenanceMode() const {
        return false;
    }

    std::string help() const override {
        return repairRemovedMessage;
    }
    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    CmdRepairDatabase() : ErrmsgCommandDeprecated("repairDatabase") {}

    bool errmsgRun(OperationContext* opCtx,
                   const string& dbname,
                   const BSONObj& cmdObj,
                   string& errmsg,
                   BSONObjBuilder& result) {

        uasserted(ErrorCodes::CommandNotFound, repairRemovedMessage);
        return false;
    }
} cmdRepairDatabase;

/* drop collection */
class CmdDrop : public ErrmsgCommandDeprecated {
public:
    CmdDrop() : ErrmsgCommandDeprecated("drop") {}
    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }
    virtual bool adminOnly() const {
        return false;
    }
    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) const {
        ActionSet actions;
        actions.addAction(ActionType::dropCollection);
        out->push_back(Privilege(parseResourcePattern(dbname, cmdObj), actions));
    }
    std::string help() const override {
        return "drop a collection\n{drop : <collectionName>}";
    }


    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    virtual bool errmsgRun(OperationContext* opCtx,
                           const string& dbname,
                           const BSONObj& cmdObj,
                           string& errmsg,
                           BSONObjBuilder& result) {
        const NamespaceString nsToDrop(CommandHelpers::parseNsCollectionRequired(dbname, cmdObj));

        if (nsToDrop.isVirtualized()) {
            errmsg = "can't drop a virtual collection";
            return false;
        }

        if ((repl::ReplicationCoordinator::get(opCtx)->getReplicationMode() !=
             repl::ReplicationCoordinator::modeNone) &&
            nsToDrop.isOplog()) {
            errmsg = "can't drop live oplog while replicating";
            return false;
        }

        uassertStatusOK(
            dropCollection(opCtx,
                           nsToDrop,
                           result,
                           {},
                           DropCollectionSystemCollectionMode::kDisallowSystemCollectionDrops));
        return true;
    }

} cmdDrop;

/* create collection */
class CmdCreate : public BasicCommand {
public:
    CmdCreate() : BasicCommand("create") {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    virtual bool adminOnly() const {
        return false;
    }

    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    std::string help() const override {
        return str::stream()
            << "explicitly creates a collection or view\n"
            << "{\n"
            << "  create: <string: collection or view name> [,\n"
            << "  capped: <bool: capped collection>,\n"
            << "  autoIndexId: <bool: automatic creation of _id index>,\n"
            << "  idIndex: <document: _id index specification>,\n"
            << "  size: <int: size in bytes of the capped collection>,\n"
            << "  max: <int: max number of documents in the capped collection>,\n"
            << "  storageEngine: <document: storage engine configuration>,\n"
            << "  validator: <document: validation rules>,\n"
            << "  validationLevel: <string: validation level>,\n"
            << "  validationAction: <string: validation action>,\n"
            << "  indexOptionDefaults: <document: default configuration for indexes>,\n"
            << "  viewOn: <string: name of source collection or view>,\n"
            << "  pipeline: <array<object>: aggregation pipeline stage>,\n"
            << "  collation: <document: default collation for the collection or view>,\n"
            << "  writeConcern: <document: write concern expression for the operation>]\n"
            << "}";
    }

    virtual Status checkAuthForCommand(Client* client,
                                       const std::string& dbname,
                                       const BSONObj& cmdObj) const {
        const NamespaceString nss(parseNs(dbname, cmdObj));
        return AuthorizationSession::get(client)->checkAuthForCreate(nss, cmdObj, false);
    }

    virtual bool run(OperationContext* opCtx,
                     const string& dbname,
                     const BSONObj& cmdObj,
                     BSONObjBuilder& result) {
        IDLParserErrorContext ctx("create");
        CreateCommand cmd = CreateCommand::parse(ctx, cmdObj);

        const NamespaceString ns = cmd.getNamespace();

        if (cmd.getAutoIndexId()) {
            const char* deprecationWarning =
                "the autoIndexId option is deprecated and will be removed in a future release";
            warning() << deprecationWarning;
            result.append("note", deprecationWarning);
        }

        // Ensure that the 'size' field is present if 'capped' is set to true.
        if (cmd.getCapped()) {
            uassert(ErrorCodes::InvalidOptions,
                    str::stream() << "the 'size' field is required when 'capped' is true",
                    cmd.getSize());
        }

        // If the 'size' or 'max' fields are present, then 'capped' must be set to true.
        if (cmd.getSize() || cmd.getMax()) {
            uassert(ErrorCodes::InvalidOptions,
                    str::stream() << "the 'capped' field needs to be true when either the 'size'"
                                  << " or 'max' fields are present",
                    cmd.getCapped());
        }

        // Validate _id index spec and fill in missing fields.
        if (cmd.getIdIndex()) {
            auto idIndexSpec = *cmd.getIdIndex();

            uassert(ErrorCodes::InvalidOptions,
                    str::stream() << "'idIndex' is not allowed with 'viewOn': " << idIndexSpec,
                    !cmd.getViewOn());

            uassert(ErrorCodes::InvalidOptions,
                    str::stream() << "'idIndex' is not allowed with 'autoIndexId': " << idIndexSpec,
                    !cmd.getAutoIndexId());

            // Perform index spec validation.
            idIndexSpec = uassertStatusOK(index_key_validate::validateIndexSpec(
                opCtx, idIndexSpec, ns, serverGlobalParams.featureCompatibility));
            uassertStatusOK(index_key_validate::validateIdIndexSpec(idIndexSpec));

            // Validate or fill in _id index collation.
            std::unique_ptr<CollatorInterface> defaultCollator;
            if (cmd.getCollation()) {
                auto collatorStatus = CollatorFactoryInterface::get(opCtx->getServiceContext())
                                          ->makeFromBSON(*cmd.getCollation());
                uassertStatusOK(collatorStatus.getStatus());
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
                uasserted(ErrorCodes::BadValue,
                          "'idIndex' must have the same collation as the collection.");
            }

            // Remove "idIndex" field from command.
            auto resolvedCmdObj = cmdObj.removeField("idIndex");

            uassertStatusOK(createCollection(opCtx, dbname, resolvedCmdObj, idIndexSpec));
            return true;
        }

        BSONObj idIndexSpec;
        uassertStatusOK(createCollection(opCtx, dbname, cmdObj, idIndexSpec));
        return true;
    }
} cmdCreate;

class CmdDatasize : public ErrmsgCommandDeprecated {
    virtual string parseNs(const string& dbname, const BSONObj& cmdObj) const {
        return CommandHelpers::parseNsFullyQualified(cmdObj);
    }

public:
    CmdDatasize() : ErrmsgCommandDeprecated("dataSize", "datasize") {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }
    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }
    std::string help() const override {
        return "determine data size for a set of data in a certain range"
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
                                       std::vector<Privilege>* out) const {
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

            const IndexDescriptor* idx =
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

        if (PlanExecutor::FAILURE == state) {
            warning() << "Internal error while reading " << ns;
            uassertStatusOK(WorkingSetCommon::getMemberObjectStatus(obj).withContext(
                "Executor error while reading during dataSize command"));
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

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }
    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }
    std::string help() const override {
        return "{ collStats:\"blog.posts\" , scale : 1 } scale divides sizes e.g. for KB use 1024\n"
               "    avgObjSize - in bytes";
    }

    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) const {
        ActionSet actions;
        actions.addAction(ActionType::collStats);
        out->push_back(Privilege(parseResourcePattern(dbname, cmdObj), actions));
    }

    bool errmsgRun(OperationContext* opCtx,
                   const string& dbname,
                   const BSONObj& jsobj,
                   string& errmsg,
                   BSONObjBuilder& result) {
        const NamespaceString nss(CommandHelpers::parseNsCollectionRequired(dbname, jsobj));

        if (nss.coll().empty()) {
            errmsg = "No collection name specified";
            return false;
        }

        result.append("ns", nss.ns());
        Status status = appendCollectionStorageStats(opCtx, nss, jsobj, &result);
        if (!status.isOK() && status.code() != ErrorCodes::NamespaceNotFound) {
            errmsg = status.reason();
            return false;
        }

        return true;
    }

} cmdCollectionStats;

class CollectionModCommand : public BasicCommand {
public:
    CollectionModCommand() : BasicCommand("collMod") {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }
    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }
    std::string help() const override {
        return "Sets collection options.\n"
               "Example: { collMod: 'foo', usePowerOf2Sizes:true }\n"
               "Example: { collMod: 'foo', index: {keyPattern: {a: 1}, expireAfterSeconds: 600} "
               "Example: { collMod: 'foo', index: {name: 'bar', expireAfterSeconds: 600} }\n";
    }

    virtual Status checkAuthForCommand(Client* client,
                                       const std::string& dbname,
                                       const BSONObj& cmdObj) const {
        const NamespaceString nss(parseNs(dbname, cmdObj));
        return AuthorizationSession::get(client)->checkAuthForCollMod(nss, cmdObj, false);
    }

    bool run(OperationContext* opCtx,
             const string& dbname,
             const BSONObj& jsobj,
             BSONObjBuilder& result) {
        const NamespaceString nss(CommandHelpers::parseNsCollectionRequired(dbname, jsobj));
        uassertStatusOK(collMod(opCtx, nss, jsobj, &result));
        return true;
    }

} collectionModCommand;

class DBStats : public ErrmsgCommandDeprecated {
public:
    DBStats() : ErrmsgCommandDeprecated("dbStats", "dbstats") {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }
    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }
    std::string help() const override {
        return "Get stats on a database. Not instantaneous. Slower for databases with large "
               ".ns files.\n"
               "Example: { dbStats:1, scale:1 }";
    }

    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) const {
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

        AutoGetDb autoDb(opCtx, ns, MODE_IS);

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
            if (!getGlobalServiceContext()->getStorageEngine()->isEphemeral()) {
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

class CmdBuildInfo : public BasicCommand {
public:
    CmdBuildInfo() : BasicCommand("buildInfo", "buildinfo") {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    bool requiresAuth() const override {
        return false;
    }

    virtual bool adminOnly() const {
        return false;
    }
    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }
    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) const {}  // No auth required
    std::string help() const override {
        return "get version #, etc.\n"
               "{ buildinfo:1 }";
    }

    bool run(OperationContext* opCtx,
             const std::string& dbname,
             const BSONObj& jsobj,
             BSONObjBuilder& result) {
        VersionInfoInterface::instance().appendBuildInfo(&result);
        appendStorageEngineList(opCtx->getServiceContext(), &result);
        return true;
    }

} cmdBuildInfo;

}  // namespace
}  // namespace mongo
