/**
 *    Copyright (C) 2013 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/s/write_ops/batch_downconvert.h"

#include "mongo/bson/util/builder.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/s/client/multi_command_dispatch.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"

namespace mongo {

using std::endl;
using std::string;
using std::vector;

Status extractGLEErrors(const BSONObj& gleResponse, GLEErrors* errors) {
    // DRAGONS
    // Parsing GLE responses is incredibly finicky.
    // The order of testing here is extremely important.

    ///////////////////////////////////////////////////////////////////////
    // IMPORTANT!
    // Also update extractGLEErrors in batch_api.js for any changes made here.

    const bool isOK = gleResponse["ok"].trueValue();
    const string err = gleResponse["err"].str();
    const string errMsg = gleResponse["errmsg"].str();
    const string wNote = gleResponse["wnote"].str();
    const string jNote = gleResponse["jnote"].str();
    const int code = gleResponse["code"].numberInt();
    const bool timeout = gleResponse["wtimeout"].trueValue();

    if (err == "norepl" || err == "noreplset") {
        // Know this is legacy gle and the repl not enforced - write concern error in 2.4
        errors->wcError.reset(new WriteConcernErrorDetail);
        errors->wcError->setErrCode(ErrorCodes::WriteConcernFailed);
        if (!errMsg.empty()) {
            errors->wcError->setErrMessage(errMsg);
        } else if (!wNote.empty()) {
            errors->wcError->setErrMessage(wNote);
        } else {
            errors->wcError->setErrMessage(err);
        }
    } else if (timeout) {
        // Know there was no write error
        errors->wcError.reset(new WriteConcernErrorDetail);
        errors->wcError->setErrCode(ErrorCodes::WriteConcernFailed);
        if (!errMsg.empty()) {
            errors->wcError->setErrMessage(errMsg);
        } else {
            errors->wcError->setErrMessage(err);
        }
        errors->wcError->setErrInfo(BSON("wtimeout" << true));
    } else if (code == 10990 /* no longer primary */
               ||
               code == 16805 /* replicatedToNum no longer primary */
               ||
               code == 14830 /* gle wmode changed / invalid */
               // 2.6 Error codes
               ||
               code == ErrorCodes::NotMaster || code == ErrorCodes::UnknownReplWriteConcern ||
               code == ErrorCodes::WriteConcernFailed) {
        // Write concern errors that get returned as regular errors (result may not be ok: 1.0)
        errors->wcError.reset(new WriteConcernErrorDetail());
        errors->wcError->setErrCode(ErrorCodes::fromInt(code));
        errors->wcError->setErrMessage(errMsg);
    } else if (!isOK) {
        //
        // !!! SOME GLE ERROR OCCURRED, UNKNOWN WRITE RESULT !!!
        //

        return Status(DBException::convertExceptionCode(code ? code : ErrorCodes::UnknownError),
                      errMsg);
    } else if (!err.empty()) {
        // Write error
        errors->writeError.reset(new WriteErrorDetail);
        int writeErrorCode = code == 0 ? ErrorCodes::UnknownError : code;

        // COMPATIBILITY
        // Certain clients expect write commands to always report 11000 for duplicate key
        // errors, while legacy GLE can return additional codes.
        if (writeErrorCode == 11001 /* dup key in update */
            ||
            writeErrorCode == 12582 /* dup key capped */) {
            writeErrorCode = ErrorCodes::DuplicateKey;
        }

        errors->writeError->setErrCode(writeErrorCode);
        errors->writeError->setErrMessage(err);
    } else if (!jNote.empty()) {
        // Know this is legacy gle and the journaling not enforced - write concern error in 2.4
        errors->wcError.reset(new WriteConcernErrorDetail);
        errors->wcError->setErrCode(ErrorCodes::WriteConcernFailed);
        errors->wcError->setErrMessage(jNote);
    }

    return Status::OK();
}

/**
 * Suppress the "err" and "code" field if they are coming from a previous write error and
 * are not related to write concern.  Also removes any write stats information (e.g. "n")
 *
 * Also, In some cases, 2.4 GLE w/ wOpTime can give us duplicate "err" and "code" fields b/c of
 * reporting a previous error.  The later field is what we want - dedup and use later field.
 *
 * Returns the stripped GLE response.
 */
BSONObj stripNonWCInfo(const BSONObj& gleResponse) {
    BSONObjIterator it(gleResponse);
    BSONObjBuilder builder;

    BSONElement codeField;  // eoo
    BSONElement errField;   // eoo

    while (it.more()) {
        BSONElement el = it.next();
        StringData fieldName(el.fieldName());
        if (fieldName.compare("err") == 0) {
            errField = el;
        } else if (fieldName.compare("code") == 0) {
            codeField = el;
        } else if (fieldName.compare("n") == 0 || fieldName.compare("nModified") == 0 ||
                   fieldName.compare("upserted") == 0 ||
                   fieldName.compare("updatedExisting") == 0) {
            // Suppress field
        } else {
            builder.append(el);
        }
    }

    if (!codeField.eoo()) {
        if (!gleResponse["ok"].trueValue()) {
            // The last code will be from the write concern
            builder.append(codeField);
        } else {
            // The code is from a non-wc error on this connection - suppress it
        }
    }

    if (!errField.eoo()) {
        string err = errField.str();
        if (err == "norepl" || err == "noreplset" || err == "timeout") {
            // Append err if it's from a write concern issue
            builder.append(errField);
        } else {
            // Suppress non-write concern err as null, but we need to report null err if ok
            if (gleResponse["ok"].trueValue())
                builder.appendNull(errField.fieldName());
        }
    }

    return builder.obj();
}

// Adds a wOpTime and a wElectionId field to a set of gle options
static BSONObj buildGLECmdWithOpTime(const BSONObj& gleOptions,
                                     const repl::OpTime& opTime,
                                     const OID& electionId) {
    BSONObjBuilder builder;
    BSONObjIterator it(gleOptions);

    for (int i = 0; it.more(); ++i) {
        BSONElement el = it.next();

        // Make sure first element is getLastError : 1
        if (i == 0) {
            StringData elName(el.fieldName());
            if (!elName.equalCaseInsensitive("getLastError")) {
                builder.append("getLastError", 1);
            }
        }

        builder.append(el);
    }
    opTime.append(&builder, "wOpTime");
    builder.appendOID("wElectionId", const_cast<OID*>(&electionId));
    return builder.obj();
}

Status enforceLegacyWriteConcern(MultiCommandDispatch* dispatcher,
                                 StringData dbName,
                                 const BSONObj& options,
                                 const HostOpTimeMap& hostOpTimes,
                                 vector<LegacyWCResponse>* legacyWCResponses) {
    if (hostOpTimes.empty()) {
        return Status::OK();
    }

    for (HostOpTimeMap::const_iterator it = hostOpTimes.begin(); it != hostOpTimes.end(); ++it) {
        const ConnectionString& shardEndpoint = it->first;
        const HostOpTime hot = it->second;
        const repl::OpTime& opTime = hot.opTime;
        const OID& electionId = hot.electionId;

        LOG(3) << "enforcing write concern " << options << " on " << shardEndpoint.toString()
               << " at opTime " << opTime.getTimestamp().toStringPretty() << " with electionID "
               << electionId;

        BSONObj gleCmd = buildGLECmdWithOpTime(options, opTime, electionId);
        dispatcher->addCommand(shardEndpoint, dbName, gleCmd);
    }

    dispatcher->sendAll();

    vector<Status> failedStatuses;

    while (dispatcher->numPending() > 0) {
        ConnectionString shardEndpoint;
        RawBSONSerializable gleResponseSerial;

        Status dispatchStatus = dispatcher->recvAny(&shardEndpoint, &gleResponseSerial);
        if (!dispatchStatus.isOK()) {
            // We need to get all responses before returning
            failedStatuses.push_back(dispatchStatus);
            continue;
        }

        BSONObj gleResponse = stripNonWCInfo(gleResponseSerial.toBSON());

        // Use the downconversion tools to determine if this GLE response is ok, a
        // write concern error, or an unknown error we should immediately abort for.
        GLEErrors errors;
        Status extractStatus = extractGLEErrors(gleResponse, &errors);
        if (!extractStatus.isOK()) {
            failedStatuses.push_back(extractStatus);
            continue;
        }

        LegacyWCResponse wcResponse;
        wcResponse.shardHost = shardEndpoint.toString();
        wcResponse.gleResponse = gleResponse;
        if (errors.wcError.get()) {
            wcResponse.errToReport = errors.wcError->getErrMessage();
        }

        legacyWCResponses->push_back(wcResponse);
    }

    if (failedStatuses.empty()) {
        return Status::OK();
    }

    StringBuilder builder;
    builder << "could not enforce write concern";

    for (vector<Status>::const_iterator it = failedStatuses.begin(); it != failedStatuses.end();
         ++it) {
        const Status& failedStatus = *it;
        if (it == failedStatuses.begin()) {
            builder << causedBy(failedStatus.toString());
        } else {
            builder << ":: and ::" << failedStatus.toString();
        }
    }

    return Status(failedStatuses.size() == 1u ? failedStatuses.front().code()
                                              : ErrorCodes::MultipleErrorsOccurred,
                  builder.str());
}

}  // namespace mongo
