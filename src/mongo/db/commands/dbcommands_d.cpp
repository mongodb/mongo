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

#include <time.h>

#include "mongo/base/simple_string_data_comparator.h"
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
#include "mongo/db/catalog/create_collection.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/drop_collection.h"
#include "mongo/db/catalog/drop_database.h"
#include "mongo/db/catalog/index_key_validate.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/commands.h"
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
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/server_parameters.h"
#include "mongo/db/stats/storage_stats.h"
#include "mongo/db/write_concern.h"
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

namespace {

/**
 * Sets the profiling level, logging/profiling threshold, and logging/profiling sample rate for the
 * given database.
*/
class CmdProfile : public ProfileCmdBase {
public:
    CmdProfile() = default;

protected:
    int _applyProfilingLevel(OperationContext* opCtx,
                             const std::string& dbName,
                             int profilingLevel) const final {
        const bool readOnly = (profilingLevel < 0 || profilingLevel > 2);
        const LockMode dbMode = readOnly ? MODE_S : MODE_X;

        AutoGetDb ctx(opCtx, dbName, dbMode);
        Database* db = ctx.getDb();

        auto oldLevel = (db ? db->getProfilingLevel() : serverGlobalParams.defaultProfile);

        if (!readOnly) {
            if (!db) {
                // When setting the profiling level, create the database if it didn't already exist.
                // When just reading the profiling level, we do not create the database.
                db = DatabaseHolder::getDatabaseHolder().openDb(opCtx, dbName);
            }
            uassertStatusOK(db->setProfilingLevel(opCtx, profilingLevel));
        }

        return oldLevel;
    }

} cmdProfile;

class CmdFileMD5 : public BasicCommand {
public:
    CmdFileMD5() : BasicCommand("filemd5") {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    std::string help() const override {
        return " example: { filemd5 : ObjectId(aaaaaaa) , root : \"fs\" }";
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
                                       std::vector<Privilege>* out) const {
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
                uassert(50847,
                        str::stream() << "The element that calls binDataClean() must be type of "
                                         "BinData, but type of "
                                      << typeName(stateElem.type())
                                      << " found.",
                        (stateElem.type() == BSONType::BinData));

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
                uassert(50848,
                        str::stream() << "The element that calls binDataClean() must be type "
                                         "of BinData, but type of misisng found. Field name is "
                                         "required",
                        owned["data"]);
                uassert(50849,
                        str::stream() << "The element that calls binDataClean() must be type "
                                         "of BinData, but type of "
                                      << owned["data"].type()
                                      << " found.",
                        owned["data"].type() == BSONType::BinData);

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
                } catch (const StaleConfigException&) {
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
                uassertStatusOK(WorkingSetCommon::getMemberObjectStatus(obj).withContext(
                    "Executor error during filemd5 command"));
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

class AvailableQueryOptions : public BasicCommand {
public:
    AvailableQueryOptions() : BasicCommand("availableQueryOptions", "availablequeryoptions") {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }
    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }
    virtual Status checkAuthForCommand(Client* client,
                                       const std::string& dbname,
                                       const BSONObj& cmdObj) const {
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

}  // namespace
}  // namespace mongo
