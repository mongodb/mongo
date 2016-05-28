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

#include "mongo/platform/basic.h"

#include <vector>

#include "mongo/client/remote_command_targeter.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/lasterror.h"
#include "mongo/s/client/dbclient_multi_command.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/cluster_last_error_info.h"
#include "mongo/s/grid.h"
#include "mongo/s/write_ops/batch_downconvert.h"

namespace mongo {
namespace {

class GetLastErrorCmd : public Command {
public:
    GetLastErrorCmd() : Command("getLastError", false, "getlasterror") {}


    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    virtual bool slaveOk() const {
        return true;
    }

    virtual void help(std::stringstream& help) const {
        help << "check for an error on the last command executed";
    }

    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) {
        // No auth required for getlasterror
    }

    virtual bool run(OperationContext* txn,
                     const std::string& dbname,
                     BSONObj& cmdObj,
                     int options,
                     std::string& errmsg,
                     BSONObjBuilder& result) {
        // Mongos GLE - finicky.
        //
        // To emulate mongod, we first append any write errors we had, then try to append
        // write concern error if there was no write error.  We need to contact the previous
        // shards regardless to maintain 2.4 behavior.
        //
        // If there are any unexpected or connectivity errors when calling GLE, fail the
        // command.
        //
        // Finally, report the write concern errors IF we don't already have an error.
        // If we only get one write concern error back, report that, otherwise report an
        // aggregated error.
        //
        // TODO: Do we need to contact the prev shards regardless - do we care that much
        // about 2.4 behavior?
        //

        LastError* le = &LastError::get(cc());
        le->disable();


        // Write commands always have the error stored in the mongos last error
        bool errorOccurred = false;
        if (le->getNPrev() == 1) {
            errorOccurred = le->appendSelf(result, false);
        }

        // For compatibility with 2.4 sharded GLE, we always enforce the write concern
        // across all shards.
        const HostOpTimeMap hostOpTimes(ClusterLastErrorInfo::get(cc()).getPrevHostOpTimes());
        HostOpTimeMap resolvedHostOpTimes;

        Status status(Status::OK());
        for (HostOpTimeMap::const_iterator it = hostOpTimes.begin(); it != hostOpTimes.end();
             ++it) {
            const ConnectionString& shardEndpoint = it->first;
            const HostOpTime& hot = it->second;

            const ReadPreferenceSetting readPref(ReadPreference::PrimaryOnly, TagSet());
            auto shard = grid.shardRegistry()->getShard(txn, shardEndpoint.toString());
            if (!shard) {
                status =
                    Status(ErrorCodes::ShardNotFound,
                           str::stream() << "shard " << shardEndpoint.toString() << " not found");
                break;
            }
            auto swHostAndPort = shard->getTargeter()->findHost(readPref);
            if (!swHostAndPort.isOK()) {
                status = swHostAndPort.getStatus();
                break;
            }

            ConnectionString resolvedHost(swHostAndPort.getValue());

            resolvedHostOpTimes[resolvedHost] = hot;
        }

        DBClientMultiCommand dispatcher;
        std::vector<LegacyWCResponse> wcResponses;
        if (status.isOK()) {
            status = enforceLegacyWriteConcern(
                &dispatcher, dbname, cmdObj, resolvedHostOpTimes, &wcResponses);
        }

        // Don't forget about our last hosts, reset the client info
        ClusterLastErrorInfo::get(cc()).disableForCommand();

        // We're now done contacting all remote servers, just report results

        if (!status.isOK()) {
            // Return immediately if we failed to contact a shard, unexpected GLE issue
            // Can't return code, since it may have been set above (2.4 compatibility)
            result.append("errmsg", status.reason());
            return false;
        }

        // Go through all the write concern responses and find errors
        BSONArrayBuilder shards;
        BSONObjBuilder shardRawGLE;
        BSONArrayBuilder errors;
        BSONArrayBuilder errorRawGLE;

        int numWCErrors = 0;
        const LegacyWCResponse* lastErrResponse = NULL;

        for (std::vector<LegacyWCResponse>::const_iterator it = wcResponses.begin();
             it != wcResponses.end();
             ++it) {
            const LegacyWCResponse& wcResponse = *it;

            shards.append(wcResponse.shardHost);
            shardRawGLE.append(wcResponse.shardHost, wcResponse.gleResponse);

            if (!wcResponse.errToReport.empty()) {
                numWCErrors++;
                lastErrResponse = &wcResponse;
                errors.append(wcResponse.errToReport);
                errorRawGLE.append(wcResponse.gleResponse);
            }
        }

        // Always report what we found to match 2.4 behavior and for debugging
        if (wcResponses.size() == 1u) {
            result.append("singleShard", wcResponses.front().shardHost);
        } else {
            result.append("shards", shards.arr());
            result.append("shardRawGLE", shardRawGLE.obj());
        }

        // Suppress write concern errors if a write error occurred, to match mongod behavior
        if (errorOccurred || numWCErrors == 0) {
            // Still need to return err
            if (!errorOccurred) {
                result.appendNull("err");
            }

            return true;
        }

        if (numWCErrors == 1) {
            // Return the single write concern error we found, err should be set or not
            // from gle response
            result.appendElements(lastErrResponse->gleResponse);
            return lastErrResponse->gleResponse["ok"].trueValue();
        } else {
            // Return a generic combined WC error message
            result.append("errs", errors.arr());
            result.append("errObjects", errorRawGLE.arr());

            // Need to always return err
            result.appendNull("err");

            return appendCommandStatus(
                result,
                Status(ErrorCodes::WriteConcernFailed, "multiple write concern errors occurred"));
        }
    }

} cmdGetLastError;

}  // namespace
}  // namespace mongo
