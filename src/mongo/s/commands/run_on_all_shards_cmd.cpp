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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include "mongo/s/commands/run_on_all_shards_cmd.h"

#include <list>
#include <set>

#include <boost/shared_ptr.hpp>

#include "mongo/db/jsobj.h"
#include "mongo/client/parallel.h"
#include "mongo/s/shard.h"
#include "mongo/util/log.h"

namespace mongo {

    RunOnAllShardsCommand::RunOnAllShardsCommand(const char* name,
                                                 const char* oldName,
                                                 bool useShardConn)
        : Command(name, false, oldName)
        , _useShardConn(useShardConn)
    {}

    void RunOnAllShardsCommand::aggregateResults(const std::vector<BSONObj>& results,
                                                 BSONObjBuilder& output)
    {}

    BSONObj RunOnAllShardsCommand::specialErrorHandler(const std::string& server,
                                                       const std::string& db,
                                                       const BSONObj& cmdObj,
                                                       const BSONObj& originalResult) const {
        return originalResult;
    }

    void RunOnAllShardsCommand::getShards(const std::string& db,
                                          BSONObj& cmdObj,
                                          std::set<Shard>& shards) {
        std::vector<Shard> shardList;
        Shard::getAllShards(shardList);
        shards.insert(shardList.begin(), shardList.end());
    }

    bool RunOnAllShardsCommand::run(OperationContext* txn,
                                    const std::string& dbName,
                                    BSONObj& cmdObj,
                                    int options,
                                    std::string& errmsg,
                                    BSONObjBuilder& output,
                                    bool fromRepl) {

        LOG(1) << "RunOnAllShardsCommand db: " << dbName << " cmd:" << cmdObj;
        std::set<Shard> shards;
        getShards(dbName, cmdObj, shards);

        // TODO: Future is deprecated, replace with commandOp()

        std::list< boost::shared_ptr<Future::CommandResult> > futures;
        for (std::set<Shard>::const_iterator i=shards.begin(), end=shards.end() ; i != end ; i++) {
            futures.push_back( Future::spawnCommand( i->getConnString(),
                                                     dbName,
                                                     cmdObj,
                                                     0,
                                                     NULL,
                                                     _useShardConn ));
        }

        std::vector<BSONObj> results;
        BSONObjBuilder subobj (output.subobjStart("raw"));
        BSONObjBuilder errors;
        int commonErrCode = -1;

        for ( std::list< boost::shared_ptr<Future::CommandResult> >::iterator i=futures.begin();
              i!=futures.end(); i++ ) {

            boost::shared_ptr<Future::CommandResult> res = *i;

            if ( res->join() ) {
                // success :)
                BSONObj result = res->result();
                results.push_back( result );
                subobj.append( res->getServer(), result );
                continue;
            }

            BSONObj result = res->result();

            if ( result["errmsg"].type() ||
                 result["code"].numberInt() != 0 ) {
                result = specialErrorHandler( res->getServer(), dbName, cmdObj, result );

                BSONElement errmsg = result["errmsg"];
                if ( errmsg.eoo() || errmsg.String().empty() ) {
                    // it was fixed!
                    results.push_back( result );
                    subobj.append( res->getServer(), result );
                    continue;
                }
            }

            // Handle "errmsg".
            if( ! result["errmsg"].eoo() ){
                errors.appendAs(result["errmsg"], res->getServer());
            }
            else {
                // Can happen if message is empty, for some reason
                errors.append( res->getServer(), str::stream() <<
                               "result without error message returned : " << result );
            }

            // Handle "code".
            int errCode = result["code"].numberInt();
            if ( commonErrCode == -1 ) {
                commonErrCode = errCode;
            }
            else if ( commonErrCode != errCode ) {
                commonErrCode = 0;
            }

            results.push_back( result );
            subobj.append( res->getServer(), result );
        }

        subobj.done();

        BSONObj errobj = errors.done();
        if (! errobj.isEmpty()) {
            errmsg = errobj.toString(false, true);

            // If every error has a code, and the code for all errors is the same, then add
            // a top-level field "code" with this value to the output object.
            if ( commonErrCode > 0 ) {
                output.append( "code", commonErrCode );
            }

            return false;
        }

        aggregateResults(results, output);
        return true;
    }

}
