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

#include "mongo/db/repl/repl_set_declare_election_winner_args.h"

#include "mongo/bson/util/bson_check.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/jsobj.h"

namespace mongo {
namespace repl {
namespace {

const std::string kCommandName = "replSetDeclareElectionWinner";
const std::string kErrorMessageFieldName = "errmsg";
const std::string kErrorCodeFieldName = "code";
const std::string kOkFieldName = "ok";
const std::string kSetNameFieldName = "setName";
const std::string kTermFieldName = "term";
const std::string kWinnerIdFieldName = "winnerId";

const std::string kLegalArgsFieldNames[] = {
    kCommandName, kSetNameFieldName, kTermFieldName, kWinnerIdFieldName,
};

const std::string kLegalResponseFieldNames[] = {
    kErrorMessageFieldName, kErrorCodeFieldName, kOkFieldName, kTermFieldName,
};

}  // namespace

Status ReplSetDeclareElectionWinnerArgs::initialize(const BSONObj& argsObj) {
    Status status =
        bsonCheckOnlyHasFields("ReplSetDeclareElectionWinner", argsObj, kLegalArgsFieldNames);
    if (!status.isOK())
        return status;

    status = bsonExtractStringField(argsObj, kSetNameFieldName, &_setName);
    if (!status.isOK())
        return status;

    status = bsonExtractIntegerField(argsObj, kTermFieldName, &_term);
    if (!status.isOK())
        return status;

    status = bsonExtractIntegerField(argsObj, kWinnerIdFieldName, &_winnerId);
    if (!status.isOK())
        return status;

    return Status::OK();
}

std::string ReplSetDeclareElectionWinnerArgs::getReplSetName() const {
    return _setName;
}

long long ReplSetDeclareElectionWinnerArgs::getTerm() const {
    return _term;
}

long long ReplSetDeclareElectionWinnerArgs::getWinnerId() const {
    return _winnerId;
}

void ReplSetDeclareElectionWinnerArgs::addToBSON(BSONObjBuilder* builder) const {
    builder->append("replSetDeclareElectionWinner", 1);
    builder->append(kSetNameFieldName, _setName);
    builder->append(kTermFieldName, _term);
    builder->appendIntOrLL(kWinnerIdFieldName, _winnerId);
}

BSONObj ReplSetDeclareElectionWinnerArgs::toBSON() const {
    BSONObjBuilder builder;
    addToBSON(&builder);
    return builder.obj();
}

Status ReplSetDeclareElectionWinnerResponse::initialize(const BSONObj& argsObj) {
    Status status =
        bsonCheckOnlyHasFields("ReplSetDeclareElectionWinner", argsObj, kLegalResponseFieldNames);
    if (!status.isOK())
        return status;

    status = bsonExtractIntegerField(argsObj, kTermFieldName, &_term);
    if (!status.isOK())
        return status;

    status =
        bsonExtractIntegerFieldWithDefault(argsObj, kErrorCodeFieldName, ErrorCodes::OK, &_code);
    if (!status.isOK())
        return status;

    status = bsonExtractStringFieldWithDefault(argsObj, kErrorMessageFieldName, "", &_errmsg);
    if (!status.isOK())
        return status;

    status = bsonExtractBooleanField(argsObj, kOkFieldName, &_ok);
    if (!status.isOK())
        return status;

    return Status::OK();
}

bool ReplSetDeclareElectionWinnerResponse::getOk() const {
    return _ok;
}

long long ReplSetDeclareElectionWinnerResponse::getTerm() const {
    return _term;
}

long long ReplSetDeclareElectionWinnerResponse::getErrorCode() const {
    return _code;
}

const std::string& ReplSetDeclareElectionWinnerResponse::getErrorMsg() const {
    return _errmsg;
}

void ReplSetDeclareElectionWinnerResponse::addToBSON(BSONObjBuilder* builder) const {
    builder->append(kOkFieldName, _ok);
    builder->append(kTermFieldName, _term);
    if (_code != ErrorCodes::OK) {
        builder->append(kErrorCodeFieldName, _code);
        builder->append(kErrorMessageFieldName, _errmsg);
    }
}

}  // namespace repl
}  // namespace mongo
