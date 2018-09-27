/*
 *    Copyright (C) 2017 MongoDB, Inc.
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

#include "mongo/base/disallow_copying.h"
#include "mongo/db/logical_session_id.h"
#include "mongo/util/decorable.h"

namespace mongo {

class OperationContext;

/**
 * A decorable container for state associated with an active session running on a MongoD or MongoS
 * server. Refer to SessionCatalog for more information on the semantics of sessions.
 */
class Session : public Decorable<Session> {
    MONGO_DISALLOW_COPYING(Session);

public:
    explicit Session(LogicalSessionId sessionId);

    /**
     * The logical session id that this object represents.
     */
    const LogicalSessionId& getSessionId() const {
        return _sessionId;
    }

    /**
     * Sets the current operation running on this Session.
     */
    void setCurrentOperation(OperationContext* currentOperation);

    /**
     * Clears the current operation running on this Session.
     */
    void clearCurrentOperation();

    /**
     * Returns a pointer to the current operation running on this Session, or nullptr if there is no
     * operation currently running on this Session.
     */
    OperationContext* getCurrentOperation() const;

private:
    // The id of the session with which this object is associated
    const LogicalSessionId _sessionId;

    // Protects the member variables below.
    mutable stdx::mutex _mutex;

    // A pointer back to the currently running operation on this Session, or nullptr if there
    // is no operation currently running for the Session.
    OperationContext* _currentOperation{nullptr};
};

}  // namespace mongo
