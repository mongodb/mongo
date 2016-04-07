// lasterror.cpp

/*    Copyright 2009 10gen Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/lasterror.h"

#include "mongo/db/jsobj.h"
#include "mongo/util/assert_util.h"

namespace mongo {

LastError LastError::noError;

const Client::Decoration<LastError> LastError::get = Client::declareDecoration<LastError>();

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
}

void LastError::recordUpdate(bool updateObjects, long long nObjects, BSONObj upsertedId) {
    reset(true);
    _nObjects = nObjects;
    _updatedExisting = updateObjects ? True : False;
    if (upsertedId.valid() && upsertedId.hasField(kUpsertedFieldName))
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
    }

    if (_code)
        b.append("code", _code);
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
}

}  // namespace mongo
