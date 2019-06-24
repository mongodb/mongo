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

#include "mongo/platform/basic.h"

#include <boost/algorithm/string.hpp>

#include "mongo/db/lasterror.h"

#include "mongo/db/jsobj.h"
#include "mongo/util/assert_util.h"

namespace mongo {

LastError LastError::noError;

const Client::Decoration<LastError> LastError::get = Client::declareDecoration<LastError>();

namespace {
void appendDupKeyFields(BSONObjBuilder& builder, std::string errMsg) {
    // errMsg format for duplicate key errors:
    // "E11000 duplicate key error collection: test.coll index: a_1 dup key: { a: 1.0 }",
    std::vector<std::string> results;
    boost::split(results, errMsg, [](char c) { return c == ' '; });
    auto collName = results[5];
    auto indexName = results[7];
    builder.append("ns", collName);
    builder.append("index", indexName);
}
}

void LastError::reset(bool valid) {
    *this = LastError();
    _valid = valid;
}

void LastError::setLastError(int code, std::string msg) {
    if (_disabled) {
        return;
    }
    reset(true);
    _code = code;
    _msg = std::move(msg);

    if (ErrorCodes::isNotMasterError(ErrorCodes::Error(_code)))
        _hadNotMasterError = true;
}

void LastError::recordUpdate(bool updateObjects, long long nObjects, BSONObj upsertedId) {
    reset(true);
    _nObjects = nObjects;
    _updatedExisting = updateObjects ? True : False;

    // Use the latest BSON validation version. We record updates containing decimal data even if
    // decimal is disabled.
    if (upsertedId.valid(BSONVersion::kLatest) && upsertedId.hasField(kUpsertedFieldName))
        _upsertedId = upsertedId;
}

void LastError::recordDelete(long long nDeleted) {
    reset(true);
    _nObjects = nDeleted;
}

bool LastError::appendSelf(BSONObjBuilder& b, bool blankErr) const {
    if (!_valid) {
        if (blankErr)
            b.appendNull("err");
        b.append("n", 0);
        return false;
    }

    if (_msg.empty()) {
        if (blankErr) {
            b.appendNull("err");
        }
    } else {
        b.append("err", _msg);
        if (_msg.find("E11000 duplicate key error") != std::string::npos) {
            appendDupKeyFields(b, _msg);
        }
    }

    if (_code) {
        b.append("code", _code);
        b.append("codeName", ErrorCodes::errorString(ErrorCodes::Error(_code)));
    }
    if (_updatedExisting != NotUpdate)
        b.appendBool("updatedExisting", _updatedExisting == True);
    if (!_upsertedId.isEmpty()) {
        b.append(_upsertedId[kUpsertedFieldName]);
    }
    b.appendNumber("n", _nObjects);

    return !_msg.empty();
}


void LastError::disable() {
    invariant(!_disabled);
    _disabled = true;
    _nPrev--;  // caller is a command that shouldn't count as an operation
}

void LastError::startRequest() {
    _disabled = false;
    ++_nPrev;
    _hadNotMasterError = false;
}

}  // namespace mongo
