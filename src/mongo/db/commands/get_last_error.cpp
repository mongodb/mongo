// get_last_error.cpp

/**
*    Copyright (C) 2013 10gen Inc.
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

#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/curop.h"
#include "mongo/db/field_parser.h"
#include "mongo/db/lasterror.h"
#include "mongo/db/repl/bson_extract_optime.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/write_concern.h"
#include "mongo/util/log.h"

namespace mongo {
namespace {

using std::string;
using std::stringstream;

/* reset any errors so that getlasterror comes back clean.

   useful before performing a long series of operations where we want to
   see if any of the operations triggered an error, but don't want to check
   after each op as that woudl be a client/server turnaround.
*/
class CmdResetError : public Command {
public:
    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }
    virtual bool slaveOk() const {
        return true;
    }
    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) {}  // No auth required
    virtual void help(stringstream& help) const {
        help << "reset error state (used with getpreverror)";
    }
    CmdResetError() : Command("resetError", false, "reseterror") {}
    bool run(OperationContext* txn,
             const string& db,
             BSONObj& cmdObj,
             int,
             string& errmsg,
             BSONObjBuilder& result) {
        LastError::get(txn->getClient()).reset();
        return true;
    }
} cmdResetError;

class CmdGetLastError : public Command {
public:
    CmdGetLastError() : Command("getLastError", false, "getlasterror") {}
    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }
    virtual bool slaveOk() const {
        return true;
    }
    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) {}  // No auth required
    virtual void help(stringstream& help) const {
        help << "return error status of the last operation on this connection\n"
             << "options:\n"
             << "  { fsync:true } - fsync before returning, or wait for journal commit if running "
                "with --journal\n"
             << "  { j:true } - wait for journal commit if running with --journal\n"
             << "  { w:n } - await replication to n servers (including self) before returning\n"
             << "  { w:'majority' } - await replication to majority of set\n"
             << "  { wtimeout:m} - timeout for w in m milliseconds";
    }

    bool run(OperationContext* txn,
             const string& dbname,
             BSONObj& cmdObj,
             int,
             string& errmsg,
             BSONObjBuilder& result) {
        //
        // Correct behavior here is very finicky.
        //
        // 1.  The first step is to append the error that occurred on the previous operation.
        // This adds an "err" field to the command, which is *not* the command failing.
        //
        // 2.  Next we parse and validate write concern options.  If these options are invalid
        // the command fails no matter what, even if we actually had an error earlier.  The
        // reason for checking here is to match legacy behavior on these kind of failures -
        // we'll still get an "err" field for the write error.
        //
        // 3.  If we had an error on the previous operation, we then return immediately.
        //
        // 4.  Finally, we actually enforce the write concern.  All errors *except* timeout are
        // reported with ok : 0.0, to match legacy behavior.
        //
        // There is a special case when "wOpTime" and "wElectionId" are explicitly provided by
        // the client (mongos) - in this case we *only* enforce the write concern if it is
        // valid.
        //
        // We always need to either report "err" (if ok : 1) or "errmsg" (if ok : 0), even if
        // err is null.
        //

        LastError* le = &LastError::get(txn->getClient());
        le->disable();

        // Always append lastOp and connectionId
        Client& c = *txn->getClient();
        auto replCoord = repl::getGlobalReplicationCoordinator();
        if (replCoord->getReplicationMode() == repl::ReplicationCoordinator::modeReplSet) {
            const repl::OpTime lastOp = repl::ReplClientInfo::forClient(c).getLastOp();
            if (!lastOp.isNull()) {
                if (replCoord->isV1ElectionProtocol()) {
                    lastOp.append(&result, "lastOp");
                } else {
                    result.append("lastOp", lastOp.getTimestamp());
                }
            }
        }

        // for sharding; also useful in general for debugging
        result.appendNumber("connectionId", c.getConnectionId());

        repl::OpTime lastOpTime;
        bool lastOpTimePresent = true;
        const BSONElement opTimeElement = cmdObj["wOpTime"];
        if (opTimeElement.eoo()) {
            lastOpTimePresent = false;
            lastOpTime = repl::ReplClientInfo::forClient(c).getLastOp();
        } else if (opTimeElement.type() == bsonTimestamp) {
            lastOpTime = repl::OpTime(opTimeElement.timestamp(), repl::OpTime::kUninitializedTerm);
        } else if (opTimeElement.type() == Date) {
            lastOpTime =
                repl::OpTime(Timestamp(opTimeElement.date()), repl::OpTime::kUninitializedTerm);
        } else if (opTimeElement.type() == Object) {
            Status status = bsonExtractOpTimeField(cmdObj, "wOpTime", &lastOpTime);
            if (!status.isOK()) {
                result.append("badGLE", cmdObj);
                return appendCommandStatus(result, status);
            }
        } else {
            return appendCommandStatus(
                result,
                Status(ErrorCodes::TypeMismatch,
                       str::stream() << "Expected \"wOpTime\" field in getLastError to "
                                        "have type Date, Timestamp, or OpTime but found type "
                                     << typeName(opTimeElement.type())));
        }


        OID electionId;
        BSONField<OID> wElectionIdField("wElectionId");
        FieldParser::FieldState extracted =
            FieldParser::extract(cmdObj, wElectionIdField, &electionId, &errmsg);
        if (!extracted) {
            result.append("badGLE", cmdObj);
            appendCommandStatus(result, false, errmsg);
            return false;
        }

        bool electionIdPresent = extracted != FieldParser::FIELD_NONE;
        bool errorOccurred = false;

        // Errors aren't reported when wOpTime is used
        if (!lastOpTimePresent) {
            if (le->getNPrev() != 1) {
                errorOccurred = LastError::noError.appendSelf(result, false);
            } else {
                errorOccurred = le->appendSelf(result, false);
            }
        }

        BSONObj writeConcernDoc = cmdObj;
        // Use the default options if we have no gle options aside from wOpTime/wElectionId
        const int nFields = cmdObj.nFields();
        bool useDefaultGLEOptions = (nFields == 1) || (nFields == 2 && lastOpTimePresent) ||
            (nFields == 3 && lastOpTimePresent && electionIdPresent);

        WriteConcernOptions writeConcern;

        if (useDefaultGLEOptions) {
            writeConcern = repl::getGlobalReplicationCoordinator()->getGetLastErrorDefault();
        }

        Status status = writeConcern.parse(writeConcernDoc);

        //
        // Validate write concern no matter what, this matches 2.4 behavior
        //
        if (status.isOK()) {
            // Ensure options are valid for this host
            status = validateWriteConcern(txn, writeConcern);
        }

        if (!status.isOK()) {
            result.append("badGLE", writeConcernDoc);
            return appendCommandStatus(result, status);
        }

        // Don't wait for replication if there was an error reported - this matches 2.4 behavior
        if (errorOccurred) {
            dassert(!lastOpTimePresent);
            return true;
        }

        // No error occurred, so we won't duplicate these fields with write concern errors
        dassert(result.asTempObj()["err"].eoo());
        dassert(result.asTempObj()["code"].eoo());

        // If we got an electionId, make sure it matches
        if (electionIdPresent) {
            if (repl::getGlobalReplicationCoordinator()->getReplicationMode() !=
                repl::ReplicationCoordinator::modeReplSet) {
                // Ignore electionIds of 0 from mongos.
                if (electionId != OID()) {
                    errmsg = "wElectionId passed but no replication active";
                    result.append("code", ErrorCodes::BadValue);
                    return false;
                }
            } else {
                if (electionId != repl::getGlobalReplicationCoordinator()->getElectionId()) {
                    LOG(3) << "oid passed in is " << electionId << ", but our id is "
                           << repl::getGlobalReplicationCoordinator()->getElectionId();
                    errmsg = "election occurred after write";
                    result.append("code", ErrorCodes::WriteConcernFailed);
                    return false;
                }
            }
        }

        {
            stdx::lock_guard<Client> lk(*txn->getClient());
            txn->setMessage_inlock("waiting for write concern");
        }

        WriteConcernResult wcResult;
        status = waitForWriteConcern(txn, lastOpTime, writeConcern, &wcResult);
        wcResult.appendTo(writeConcern, &result);

        // For backward compatibility with 2.4, wtimeout returns ok : 1.0
        if (wcResult.wTimedOut) {
            dassert(!wcResult.err.empty());  // so we always report err
            dassert(!status.isOK());
            result.append("errmsg", "timed out waiting for slaves");
            result.append("code", status.code());
            return true;
        }

        return appendCommandStatus(result, status);
    }

} cmdGetLastError;

class CmdGetPrevError : public Command {
public:
    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }
    virtual void help(stringstream& help) const {
        help << "check for errors since last reseterror commandcal";
    }
    virtual bool slaveOk() const {
        return true;
    }
    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) {}  // No auth required
    CmdGetPrevError() : Command("getPrevError", false, "getpreverror") {}
    bool run(OperationContext* txn,
             const string& dbname,
             BSONObj& cmdObj,
             int,
             string& errmsg,
             BSONObjBuilder& result) {
        LastError* le = &LastError::get(txn->getClient());
        le->disable();
        le->appendSelf(result, true);
        if (le->isValid())
            result.append("nPrev", le->getNPrev());
        else
            result.append("nPrev", -1);
        return true;
    }
} cmdGetPrevError;

}  // namespace
}  // namespace mongo
