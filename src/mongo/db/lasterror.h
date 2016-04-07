// lasterror.h

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

#pragma once

#include <string>

#include "mongo/db/client.h"
#include "mongo/db/jsobj.h"

namespace mongo {
class BSONObjBuilder;

static const char kUpsertedFieldName[] = "upserted";

class LastError {
public:
    static const Client::Decoration<LastError> get;

    /**
     * Resets the object to a newly constructed state.  If "valid" is true, marks the last-error
     * object as "valid".
     */
    void reset(bool valid = false);

    /**
     * when db receives a message/request, call this
     */
    void startRequest();

    /**
     * Disables error recording for the current operation.
     */
    void disable();

    /**
     * Sets the error information for the current operation, if error recording was not
     * explicitly disabled via a call to disable() since the call to startRequest.
     */
    void setLastError(int code, std::string msg);

    void recordUpdate(bool updateObjects, long long nObjects, BSONObj upsertedId);

    void recordDelete(long long nDeleted);

    /**
     * Writes the last-error state described by this object to "b".
     *
     * If "blankErr" is true, the "err" field will be explicitly set to null in the result
     * instead of being omitted when the error string is empty.
     *
     * Returns true if there is a non-empty error message.
     */
    bool appendSelf(BSONObjBuilder& b, bool blankErr) const;

    bool isValid() const {
        return _valid;
    }
    int getNPrev() const {
        return _nPrev;
    }

    class Disabled {
    public:
        explicit Disabled(LastError* le) : _le(le), _prev(le->_disabled) {
            _le->_disabled = true;
        }

        ~Disabled() {
            _le->_disabled = _prev;
        }

    private:
        LastError* const _le;
        const bool _prev;
    };

    static LastError noError;

private:
    enum UpdatedExistingType { NotUpdate, True, False };

    int _code = 0;
    std::string _msg = {};
    UpdatedExistingType _updatedExisting = NotUpdate;
    // _id field value from inserted doc, returned as kUpsertedFieldName (above)
    BSONObj _upsertedId = {};
    long long _nObjects = 0;
    int _nPrev = 1;
    bool _valid = false;
    bool _disabled = false;
};

}  // namespace mongo
