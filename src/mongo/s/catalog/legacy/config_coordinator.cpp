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

#include "mongo/s/catalog/legacy/config_coordinator.h"

#include "mongo/base/owned_pointer_vector.h"
#include "mongo/db/field_parser.h"
#include "mongo/db/lasterror.h"
#include "mongo/db/namespace_string.h"
#include "mongo/s/client/multi_command_dispatch.h"
#include "mongo/s/set_shard_version_request.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/util/log.h"

namespace mongo {

using std::string;
using std::vector;

namespace {

/**
 * A BSON serializable object representing a setShardVersion command response.
 */
class SSVResponse : public BSONSerializable {
    MONGO_DISALLOW_COPYING(SSVResponse);

public:
    static const BSONField<int> ok;
    static const BSONField<int> errCode;
    static const BSONField<string> errMessage;


    SSVResponse() {
        clear();
    }

    bool isValid(std::string* errMsg) const {
        return _isOkSet;
    }

    BSONObj toBSON() const {
        BSONObjBuilder builder;

        if (_isOkSet)
            builder << ok(_ok);
        if (_isErrCodeSet)
            builder << errCode(_errCode);
        if (_isErrMessageSet)
            builder << errMessage(_errMessage);

        return builder.obj();
    }

    bool parseBSON(const BSONObj& source, std::string* errMsg) {
        FieldParser::FieldState result;

        result = FieldParser::extractNumber(source, ok, &_ok, errMsg);
        if (result == FieldParser::FIELD_INVALID) {
            return false;
        }
        _isOkSet = result != FieldParser::FIELD_NONE;

        result = FieldParser::extract(source, errCode, &_errCode, errMsg);
        if (result == FieldParser::FIELD_INVALID) {
            return false;
        }
        _isErrCodeSet = result != FieldParser::FIELD_NONE;

        result = FieldParser::extract(source, errMessage, &_errMessage, errMsg);
        if (result == FieldParser::FIELD_INVALID) {
            return false;
        }
        _isErrMessageSet = result != FieldParser::FIELD_NONE;

        return true;
    }

    void clear() {
        _ok = false;
        _isOkSet = false;

        _errCode = 0;
        _isErrCodeSet = false;

        _errMessage = "";
        _isErrMessageSet = false;
    }

    string toString() const {
        return toBSON().toString();
    }

    int getOk() {
        dassert(_isOkSet);
        return _ok;
    }

    void setOk(int ok) {
        _ok = ok;
        _isOkSet = true;
    }

    int getErrCode() {
        if (_isErrCodeSet) {
            return _errCode;
        } else {
            return errCode.getDefault();
        }
    }

    void setErrCode(int errCode) {
        _errCode = errCode;
        _isErrCodeSet = true;
    }

    bool isErrCodeSet() const {
        return _isErrCodeSet;
    }

    const string& getErrMessage() {
        dassert(_isErrMessageSet);
        return _errMessage;
    }

    void setErrMessage(StringData errMsg) {
        _errMessage = errMsg.toString();
        _isErrMessageSet = true;
    }

private:
    int _ok;
    bool _isOkSet;

    int _errCode;
    bool _isErrCodeSet;

    string _errMessage;
    bool _isErrMessageSet;
};

const BSONField<int> SSVResponse::ok("ok");
const BSONField<int> SSVResponse::errCode("code");
const BSONField<string> SSVResponse::errMessage("errmsg");


struct ConfigResponse {
    ConnectionString configHost;
    BatchedCommandResponse response;
};

void buildErrorFrom(const Status& status, BatchedCommandResponse* response) {
    response->setOk(false);
    response->setErrCode(static_cast<int>(status.code()));
    response->setErrMessage(status.reason());

    dassert(response->isValid(NULL));
}

bool areResponsesEqual(const BatchedCommandResponse& responseA,
                       const BatchedCommandResponse& responseB) {
    // Note: This needs to also take into account comparing responses from legacy writes
    // and write commands.

    // TODO: Better reporting of why not equal
    if (responseA.getOk() != responseB.getOk()) {
        return false;
    }

    if (responseA.getN() != responseB.getN()) {
        return false;
    }

    if (responseA.isUpsertDetailsSet()) {
        // TODO:
    }

    if (responseA.getOk()) {
        return true;
    }

    // TODO: Compare errors here

    return true;
}

bool areAllResponsesEqual(const vector<ConfigResponse*>& responses) {
    BatchedCommandResponse* lastResponse = NULL;

    for (vector<ConfigResponse*>::const_iterator it = responses.begin(); it != responses.end();
         ++it) {
        BatchedCommandResponse* response = &(*it)->response;

        if (lastResponse != NULL) {
            if (!areResponsesEqual(*lastResponse, *response)) {
                return false;
            }
        }

        lastResponse = response;
    }

    return true;
}

void combineResponses(const vector<ConfigResponse*>& responses,
                      BatchedCommandResponse* clientResponse) {
    if (areAllResponsesEqual(responses)) {
        responses.front()->response.cloneTo(clientResponse);
        return;
    }

    BSONObjBuilder builder;
    for (vector<ConfigResponse*>::const_iterator it = responses.begin(); it != responses.end();
         ++it) {
        builder.append((*it)->configHost.toString(), (*it)->response.toBSON());
    }

    clientResponse->setOk(false);
    clientResponse->setErrCode(ErrorCodes::ManualInterventionRequired);
    clientResponse->setErrMessage(
        "config write was not consistent, "
        "manual intervention may be required. "
        "config responses: " +
        builder.obj().toString());
}

}  // namespace


ConfigCoordinator::ConfigCoordinator(MultiCommandDispatch* dispatcher,
                                     const ConnectionString& configServerConnectionString)
    : _dispatcher(dispatcher), _configServerConnectionString(configServerConnectionString) {}

bool ConfigCoordinator::_checkConfigString(BatchedCommandResponse* clientResponse) {
    //
    // Send side
    //

    for (const HostAndPort& server : _configServerConnectionString.getServers()) {
        SetShardVersionRequest ssv = SetShardVersionRequest::makeForInit(
            _configServerConnectionString, "config", _configServerConnectionString);
        _dispatcher->addCommand(ConnectionString(server), "admin", ssv.toBSON());
    }

    _dispatcher->sendAll();

    //
    // Recv side
    //

    bool ssvError = false;
    while (_dispatcher->numPending() > 0) {
        ConnectionString configHost;
        SSVResponse response;

        // We've got to recv everything, no matter what - even if some failed.
        Status dispatchStatus = _dispatcher->recvAny(&configHost, &response);

        if (ssvError) {
            // record only the first failure.
            continue;
        }

        if (!dispatchStatus.isOK()) {
            ssvError = true;
            clientResponse->setOk(false);
            clientResponse->setErrCode(static_cast<int>(dispatchStatus.code()));
            clientResponse->setErrMessage(dispatchStatus.reason());
        } else if (!response.getOk()) {
            ssvError = true;
            clientResponse->setOk(false);
            clientResponse->setErrMessage(response.getErrMessage());

            if (response.isErrCodeSet()) {
                clientResponse->setErrCode(response.getErrCode());
            }
        }
    }

    return !ssvError;
}

/**
 * The core config write functionality.
 *
 * Config writes run in two passes - the first is a quick check to ensure the config servers
 * are all reachable, the second runs the actual write.
 *
 * TODO: Upgrade and move this logic to the config servers, a state machine implementation
 * is probably the next step.
 */
void ConfigCoordinator::executeBatch(const BatchedCommandRequest& clientRequest,
                                     BatchedCommandResponse* clientResponse) {
    const NamespaceString nss(clientRequest.getNS());

    // Should never use it for anything other than DBs residing on the config server
    dassert(nss.db() == "config" || nss.db() == "admin");
    dassert(clientRequest.sizeWriteOps() == 1u);

    // This is an opportunistic check that all config servers look healthy by calling
    // getLastError on each one of them. If there was some form of write/journaling error, get
    // last error would fail.
    {
        for (const HostAndPort& server : _configServerConnectionString.getServers()) {
            _dispatcher->addCommand(
                ConnectionString(server), "admin", BSON("getLastError" << true << "fsync" << true));
        }

        _dispatcher->sendAll();

        bool error = false;
        while (_dispatcher->numPending()) {
            ConnectionString host;
            RawBSONSerializable response;

            Status status = _dispatcher->recvAny(&host, &response);
            if (status.isOK()) {
                BSONObj obj = response.toBSON();

                LOG(3) << "Response " << obj.toString();

                // If the ok field is anything other than 1, count it as error
                if (!obj["ok"].trueValue()) {
                    error = true;
                    log() << "Config server check for host " << host
                          << " returned error: " << response;
                }
            } else {
                error = true;
                log() << "Config server check for host " << host
                      << " failed with status: " << status;
            }
        }

        // All responses should have been gathered by this point
        if (error) {
            clientResponse->setOk(false);
            clientResponse->setErrCode(ErrorCodes::RemoteValidationError);
            clientResponse->setErrMessage(
                "Could not verify that config servers were active"
                " and reachable before write");
            return;
        }
    }

    if (!_checkConfigString(clientResponse)) {
        return;
    }

    //
    // Do the actual writes
    //

    BatchedCommandRequest configRequest(clientRequest.getBatchType());
    clientRequest.cloneTo(&configRequest);
    configRequest.setNS(nss);

    OwnedPointerVector<ConfigResponse> responsesOwned;
    vector<ConfigResponse*>& responses = responsesOwned.mutableVector();

    //
    // Send the actual config writes
    //

    // Get as many batches as we can at once
    for (const HostAndPort& server : _configServerConnectionString.getServers()) {
        _dispatcher->addCommand(ConnectionString(server), nss.db(), configRequest.toBSON());
    }

    // Send them all out
    _dispatcher->sendAll();

    //
    // Recv side
    //

    while (_dispatcher->numPending() > 0) {
        // Get the response
        responses.push_back(new ConfigResponse());

        ConfigResponse& configResponse = *responses.back();
        Status dispatchStatus =
            _dispatcher->recvAny(&configResponse.configHost, &configResponse.response);

        if (!dispatchStatus.isOK()) {
            buildErrorFrom(dispatchStatus, &configResponse.response);
        }
    }

    combineResponses(responses, clientResponse);
}

}  // namespace mongo
