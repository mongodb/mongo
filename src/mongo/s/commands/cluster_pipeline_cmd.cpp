/**
 *    Copyright (C) 2015 MongoDB Inc.
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

#include <boost/intrusive_ptr.hpp>
#include <boost/scoped_ptr.hpp>
#include <boost/shared_ptr.hpp>
#include <string>
#include <utility>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/db/commands.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/catalog/catalog_cache.h"
#include "mongo/s/chunk_manager.h"
#include "mongo/s/client/shard_connection.h"
#include "mongo/s/commands/cluster_commands_common.h"
#include "mongo/s/config.h"
#include "mongo/s/grid.h"
#include "mongo/s/stale_exception.h"
#include "mongo/util/log.h"

namespace mongo {

    using boost::intrusive_ptr;
    using boost::scoped_ptr;
    using boost::shared_ptr;
    using std::string;
    using std::vector;

namespace {

    /**
     * Implements the aggregation (pipeline command for sharding).
     */
    class PipelineCommand : public Command {
    public:
        PipelineCommand() : Command(Pipeline::commandName, false) { }

        virtual bool slaveOk() const {
            return true;
        }

        virtual bool adminOnly() const {
            return false;
        }

        virtual bool isWriteCommandForConfigServer() const {
            return false;
        }

        virtual void help(std::stringstream& help) const {
            help << "Runs the sharded aggregation command";
        }

        // virtuals from Command
        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {

            Pipeline::addRequiredPrivileges(this, dbname, cmdObj, out);
        }

        virtual bool run(OperationContext* txn,
                         const std::string& dbname,
                         BSONObj& cmdObj,
                         int options,
                         std::string& errmsg,
                         BSONObjBuilder& result) {

            const string fullns = parseNs(dbname, cmdObj);

            auto status = grid.catalogCache()->getDatabase(dbname);
            if (!status.isOK()) {
                return appendEmptyResultSet(result, status.getStatus(), fullns);
            }

            shared_ptr<DBConfig> conf = status.getValue();

            // If the system isn't running sharded, or the target collection isn't sharded, pass
            // this on to a mongod.
            if (!conf->isShardingEnabled() || !conf->isSharded(fullns)) {
                return aggPassthrough(conf, cmdObj, result, options);
            }

            intrusive_ptr<ExpressionContext> mergeCtx =
                                        new ExpressionContext(txn, NamespaceString(fullns));
            mergeCtx->inRouter = true;
            // explicitly *not* setting mergeCtx->tempDir

            // Parse the pipeline specification
            intrusive_ptr<Pipeline> pipeline(Pipeline::parseCommand(errmsg, cmdObj, mergeCtx));
            if (!pipeline.get()) {
                // There was some parsing error
                return false;
            }

            // If the first $match stage is an exact match on the shard key, we only have to send it
            // to one shard, so send the command to that shard.
            BSONObj firstMatchQuery = pipeline->getInitialQuery();
            ChunkManagerPtr chunkMgr = conf->getChunkManager(fullns);
            BSONObj shardKeyMatches = uassertStatusOK(
                chunkMgr->getShardKeyPattern().extractShardKeyFromQuery(firstMatchQuery));

            // Don't need to split pipeline if the first $match is an exact match on shard key, but
            // we can't send the entire pipeline to one shard if there is a $out stage, since that
            // shard may not be the primary shard for the database.
            bool needSplit = shardKeyMatches.isEmpty() || pipeline->hasOutStage();

            // Split the pipeline into pieces for mongod(s) and this mongos. If needSplit is true,
            // 'pipeline' will become the merger side.
            intrusive_ptr<Pipeline> shardPipeline(needSplit ? pipeline->splitForSharded()
                                                            : pipeline);

            // Create the command for the shards. The 'fromRouter' field means produce output to
            // be merged.
            MutableDocument commandBuilder(shardPipeline->serialize());
            if (needSplit) {
                commandBuilder.setField("fromRouter", Value(true));
                commandBuilder.setField("cursor", Value(DOC("batchSize" << 0)));
            }
            else {
                commandBuilder.setField("cursor", Value(cmdObj["cursor"]));
            }

            if (cmdObj.hasField("$queryOptions")) {
                commandBuilder.setField("$queryOptions", Value(cmdObj["$queryOptions"]));
            }

            if (cmdObj.hasField(LiteParsedQuery::cmdOptionMaxTimeMS)) {
                commandBuilder.setField(LiteParsedQuery::cmdOptionMaxTimeMS,
                                        Value(cmdObj[LiteParsedQuery::cmdOptionMaxTimeMS]));
            }

            BSONObj shardedCommand = commandBuilder.freeze().toBson();
            BSONObj shardQuery = shardPipeline->getInitialQuery();

            // Run the command on the shards
            // TODO need to make sure cursors are killed if a retry is needed
            vector<Strategy::CommandResult> shardResults;
            STRATEGY->commandOp(dbname,
                                shardedCommand,
                                options,
                                fullns,
                                shardQuery,
                                &shardResults);

            if (pipeline->isExplain()) {
                // This must be checked before we start modifying result.
                uassertAllShardsSupportExplain(shardResults);

                if (needSplit) {
                    result << "splitPipeline"
                               << DOC("shardsPart" << shardPipeline->writeExplainOps()
                                   << "mergerPart" << pipeline->writeExplainOps());
                }
                else {
                    result << "splitPipeline" << BSONNULL;
                }

                BSONObjBuilder shardExplains(result.subobjStart("shards"));
                for (size_t i = 0; i < shardResults.size(); i++) {
                    shardExplains.append(shardResults[i].shardTarget.getName(),
                                         BSON("host" << shardResults[i].target.toString() <<
                                              "stages" << shardResults[i].result["stages"]));
                }

                return true;
            }

            if (!needSplit) {
                invariant(shardResults.size() == 1);
                const auto& reply = shardResults[0].result;
                storePossibleCursor(shardResults[0].target.toString(), reply);
                result.appendElements(reply);
                return reply["ok"].trueValue();
            }

            DocumentSourceMergeCursors::CursorIds cursorIds = parseCursors(shardResults, fullns);
            pipeline->addInitialSource(DocumentSourceMergeCursors::create(cursorIds, mergeCtx));

            MutableDocument mergeCmd(pipeline->serialize());
            mergeCmd["cursor"] = Value(cmdObj["cursor"]);

            if (cmdObj.hasField("$queryOptions")) {
                mergeCmd["$queryOptions"] = Value(cmdObj["$queryOptions"]);
            }

            if (cmdObj.hasField(LiteParsedQuery::cmdOptionMaxTimeMS)) {
                mergeCmd[LiteParsedQuery::cmdOptionMaxTimeMS]
                            = Value(cmdObj[LiteParsedQuery::cmdOptionMaxTimeMS]);
            }

            string outputNsOrEmpty;
            if (DocumentSourceOut* out = dynamic_cast<DocumentSourceOut*>(pipeline->output())) {
                outputNsOrEmpty = out->getOutputNs().ns();
            }

            // Run merging command on primary shard of database. Need to use ShardConnection so
            // that the merging mongod is sent the config servers on connection init.
            ShardConnection conn(conf->getPrimary().getConnString(), outputNsOrEmpty);
            BSONObj mergedResults = aggRunCommand(conn.get(),
                                                  dbname,
                                                  mergeCmd.freeze().toBson(),
                                                  options);
            conn.done();

            // Copy output from merging (primary) shard to the output object from our command.
            // Also, propagates errmsg and code if ok == false.
            result.appendElements(mergedResults);

            return mergedResults["ok"].trueValue();
        }

    private:
        DocumentSourceMergeCursors::CursorIds parseCursors(
                                const vector<Strategy::CommandResult>& shardResults,
                                const string& fullns);

        void killAllCursors(const vector<Strategy::CommandResult>& shardResults);
        void uassertAllShardsSupportExplain(const vector<Strategy::CommandResult>& shardResults);

        // These are temporary hacks because the runCommand method doesn't report the exact
        // host the command was run on which is necessary for cursor support. The exact host
        // could be different from conn->getServerAddress() for connections that map to
        // multiple servers such as for replica sets. These also take care of registering
        // returned cursors with mongos's cursorCache.
        BSONObj aggRunCommand(DBClientBase* conn,
                              const string& db,
                              BSONObj cmd,
                              int queryOptions);

        bool aggPassthrough(DBConfigPtr conf,
                            BSONObj cmd,
                            BSONObjBuilder& result,
                            int queryOptions);
    } clusterPipelineCmd;

    DocumentSourceMergeCursors::CursorIds PipelineCommand::parseCursors(
                                            const vector<Strategy::CommandResult>& shardResults,
                                            const string& fullns) {
        try {
            DocumentSourceMergeCursors::CursorIds cursors;

            for (size_t i = 0; i < shardResults.size(); i++) {
                BSONObj result = shardResults[i].result;

                if (!result["ok"].trueValue()) {
                    // If the failure of the sharded command can be accounted to a single error,
                    // throw a UserException with that error code; otherwise, throw with a
                    // location uassert code.
                    int errCode = getUniqueCodeFromCommandResults(shardResults);
                    if (errCode == 0) {
                        errCode = 17022;
                    }

                    invariant(errCode == result["code"].numberInt() || errCode == 17022);
                    uasserted(errCode, str::stream()
                        << "sharded pipeline failed on shard "
                        << shardResults[i].shardTarget.getName() << ": "
                        << result.toString());
                }

                BSONObj cursor = result["cursor"].Obj();

                massert(17023,
                        str::stream() << "shard " << shardResults[i].shardTarget.getName()
                                      << " returned non-empty first batch",
                        cursor["firstBatch"].Obj().isEmpty());

                massert(17024,
                        str::stream() << "shard " << shardResults[i].shardTarget.getName()
                                      << " returned cursorId 0",
                        cursor["id"].Long() != 0);

                massert(17025,
                        str::stream() << "shard " << shardResults[i].shardTarget.getName()
                                      << " returned different ns: " << cursor["ns"],
                        cursor["ns"].String() == fullns);

                cursors.push_back(std::make_pair(shardResults[i].target, cursor["id"].Long()));
            }

            return cursors;
        }
        catch (...) {
            // Need to clean up any cursors we successfully created on the shards
            killAllCursors(shardResults);
            throw;
        }
    }

    void PipelineCommand::uassertAllShardsSupportExplain(
                                const vector<Strategy::CommandResult>& shardResults) {

        for (size_t i = 0; i < shardResults.size(); i++) {
            uassert(17403,
                    str::stream() << "Shard " << shardResults[i].target.toString()
                                  << " failed: " << shardResults[i].result,
                    shardResults[i].result["ok"].trueValue());

            uassert(17404,
                    str::stream() << "Shard " << shardResults[i].target.toString()
                                  << " does not support $explain",
                    shardResults[i].result.hasField("stages"));
        }
    }

    void PipelineCommand::killAllCursors(const vector<Strategy::CommandResult>& shardResults) {
        // This function must ignore and log all errors. Callers expect a best-effort attempt at
        // cleanup without exceptions. If any cursors aren't cleaned up here, they will be cleaned
        // up automatically on the shard after 10 minutes anyway.

        for (size_t i = 0; i < shardResults.size(); i++) {
            try {
                BSONObj result = shardResults[i].result;
                if (!result["ok"].trueValue()) {
                    continue;
                }

                const long long cursor = result["cursor"]["id"].Long();
                if (!cursor) {
                    continue;
                }

                ScopedDbConnection conn(shardResults[i].target);
                conn->killCursor(cursor);
                conn.done();
            }
            catch (const DBException& e) {
                log() << "Couldn't kill aggregation cursor on shard: " << shardResults[i].target
                      << " due to DBException: " << e.toString();
            }
            catch (const std::exception& e) {
                log() << "Couldn't kill aggregation cursor on shard: " << shardResults[i].target
                      << " due to std::exception: " << e.what();
            }
            catch (...) {
                log() << "Couldn't kill aggregation cursor on shard: " << shardResults[i].target
                      << " due to non-exception";
            }
        }
    }

    BSONObj PipelineCommand::aggRunCommand(DBClientBase* conn,
                                           const string& db,
                                           BSONObj cmd,
                                           int queryOptions) {

        // Temporary hack. See comment on declaration for details.

        massert(17016,
                "should only be running an aggregate command here",
                str::equals(cmd.firstElementFieldName(), "aggregate"));

        scoped_ptr<DBClientCursor> cursor(conn->query(db + ".$cmd",
                                                      cmd,
                                                      -1, // nToReturn
                                                      0, // nToSkip
                                                      NULL, // fieldsToReturn
                                                      queryOptions));
        massert(17014,
                str::stream() << "aggregate command didn't return results on host: "
                              << conn->toString(),
                cursor && cursor->more());

        BSONObj result = cursor->nextSafe().getOwned();

        if (ErrorCodes::SendStaleConfig == getStatusFromCommandResult(result)) {
            throw RecvStaleConfigException("command failed because of stale config", result);
        }

        uassertStatusOK(storePossibleCursor(cursor->originalHost(), result));
        return result;
    }

    bool PipelineCommand::aggPassthrough(DBConfigPtr conf,
                                         BSONObj cmd,
                                         BSONObjBuilder& out,
                                         int queryOptions) {

        // Temporary hack. See comment on declaration for details.
        ShardConnection conn(conf->getPrimary().getConnString(), "");
        BSONObj result = aggRunCommand(conn.get(), conf->name(), cmd, queryOptions);
        conn.done();
        out.appendElements(result);
        return result["ok"].trueValue();
    }

} // namespace
} // namespace mongo
