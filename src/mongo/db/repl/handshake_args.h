/**
 *    Copyright (C) 2014 MongoDB Inc.
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

#pragma once

#include "mongo/db/jsobj.h"

namespace mongo {

class Status;

namespace repl {

/**
 * Arguments to the handshake command.
 */
class HandshakeArgs {
public:
    HandshakeArgs();

    /**
     * Initializes this HandshakeArgs from the contents of args.
     */
    Status initialize(const BSONObj& argsObj);

    /**
     * Returns true if all required fields have been initialized.
     */
    bool isInitialized() const;

    /**
     * Gets the _id of the sender in their ReplSetConfig.
     */
    long long getMemberId() const {
        return _memberId;
    }

    /**
     * Gets the unique identifier of the sender, which is used to track replication progress.
     */
    OID getRid() const {
        return _rid;
    }

    /**
     * The below methods check whether or not value in the method name has been set.
     */
    bool hasRid() {
        return _hasRid;
    };
    bool hasMemberId() {
        return _hasMemberId;
    };

    /**
     * The below methods set the value in the method name to 'newVal'.
     */
    void setRid(const OID& newVal);
    void setMemberId(long long newVal);

    /**
     * Returns a BSONified version of the object.
     * Should only be called if the mandatory fields have been set.
     * Optional fields are only included if they have been set.
     */
    BSONObj toBSON() const;

private:
    bool _hasRid;
    bool _hasMemberId;

    // look at the body of the isInitialized() function to see which fields are mandatory
    OID _rid;
    long long _memberId;
};

}  // namespace repl
}  // namespace mongo
