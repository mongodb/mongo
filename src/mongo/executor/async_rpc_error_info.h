/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#pragma once

#include "mongo/base/error_extra_info.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/stdx/variant.h"

namespace mongo {
class BSONObjBuilder;

enum class CommandErrorProvenance { kLocal, kRemote };


/**
 * Contains information to augment the 'RemoteCommandExecutionError' error code. In particular, this
 * class holds the provenance and data of the underlying error(s).
 */
class AsyncRPCErrorInfo final : public ErrorExtraInfo {
public:
    /**
     * Nested class used to describe remote errors.
     */
    class RemoteError {
    public:
        // The BSON recieved over-the-wire that encodes the remote
        // error is stored and used to construct this in-memory representation.
        explicit RemoteError(BSONObj obj)
            : _error{obj},
              _remoteCommandResult{getStatusFromCommandResult(_error)},
              _remoteCommandWriteConcernError{getWriteConcernStatusFromCommandResult(_error)},
              _remoteCommandFirstWriteError{getFirstWriteErrorStatusFromCommandResult(_error)} {
            // The buffer backing the default empty BSONObj has static duration so it is effectively
            // owned.
            invariant(_error.isOwned() || _error.objdata() == BSONObj().objdata());
            if (BSONElement errLabelsElem = _error["errorLabels"]; !errLabelsElem.eoo()) {
                _errLabels = errLabelsElem.Array();
            }
        }

        Status getRemoteCommandResult() const {
            return _remoteCommandResult;
        }

        Status getRemoteCommandWriteConcernError() const {
            return _remoteCommandWriteConcernError;
        }

        Status getRemoteCommandFirstWriteError() const {
            return _remoteCommandFirstWriteError;
        }

        BSONObj getResponseObj() const {
            return _error;
        }

        std::vector<BSONElement> getErrorLabels() const {
            return _errLabels;
        }

    private:
        const BSONObj _error;
        Status _remoteCommandResult;
        Status _remoteCommandWriteConcernError;
        Status _remoteCommandFirstWriteError;
        std::vector<BSONElement> _errLabels;
    };

    // Required member of every ErrorExtraInfo.
    static constexpr auto code = ErrorCodes::RemoteCommandExecutionError;

    // Unused and marked unreachable - the RemoteCommandExecutionError is InternalOnly and should
    // never be encoded in a BSONObj / recieved or sent over-the-wire.
    static std::shared_ptr<const ErrorExtraInfo> parse(const BSONObj& obj);

    /**
     * Construct the relevant extra info from the RemoteCommandResponse provided by the TaskExecutor
     * used to invoke the remote command.
     */
    explicit AsyncRPCErrorInfo(executor::RemoteCommandOnAnyResponse rcr)
        : _error{RemoteError(rcr.data)} {
        if (!rcr.status.isOK()) {
            _prov = CommandErrorProvenance::kLocal;
            _error = rcr.status;
            return;
        }
        _prov = CommandErrorProvenance::kRemote;
    }

    /**
     * Construct the relevant extra info from an error status - used if a remote command invokation
     * attempt fails before it reaches the TaskExecutor level.
     */
    explicit AsyncRPCErrorInfo(Status s) : _prov{CommandErrorProvenance::kLocal}, _error{s} {}

    bool isLocal() const {
        return _prov == CommandErrorProvenance::kLocal;
    }

    bool isRemote() const {
        return _prov == CommandErrorProvenance::kRemote;
    }

    const RemoteError& asRemote() const {
        return stdx::get<const RemoteError>(_error);
    }

    Status asLocal() const {
        return stdx::get<Status>(_error);
    }

    // Unused and marked unreachable - the RemoteCommandExecutionError is InternalOnly and should
    // never be encoded in a BSONObj / recieved or sent over-the-wire.
    void serialize(BSONObjBuilder* bob) const final;

private:
    CommandErrorProvenance _prov;
    stdx::variant<Status, const RemoteError> _error;
};

}  // namespace mongo
