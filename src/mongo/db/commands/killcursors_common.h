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

#include "mongo/db/commands.h"
#include "mongo/db/cursor_id.h"

namespace mongo {

/**
 * Base class for the killCursors command, which attempts to kill all given cursors.  Contains code
 * common to mongos and mongod implementations.
 */
class KillCursorsCmdBase : public BasicCommand {
public:
    KillCursorsCmdBase() : BasicCommand("killCursors") {}

    virtual ~KillCursorsCmdBase() {}

    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    bool slaveOk() const final {
        return true;
    }

    bool maintenanceOk() const final {
        return false;
    }

    bool adminOnly() const final {
        return false;
    }

    void help(std::stringstream& help) const final {
        help << "kill a list of cursor ids";
    }

    bool shouldAffectCommandCounter() const final {
        return true;
    }

    Status checkAuthForCommand(Client* client,
                               const std::string& dbname,
                               const BSONObj& cmdObj) final;

    bool run(OperationContext* opCtx,
             const std::string& dbname,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) final;

private:
    /**
     * Verify the cursor exists, is unpinned, and can be killed by the current user(s).
     */
    virtual Status _checkAuth(Client* client,
                              const NamespaceString& nss,
                              CursorId cursorId) const = 0;

    /**
     * Kill the cursor with id 'cursorId' in namespace 'nss'. Use 'opCtx' if necessary.
     *
     * Returns Status::OK() if the cursor was killed, or ErrorCodes::CursorNotFound if there is no
     * such cursor, or ErrorCodes::OperationFailed if the cursor cannot be killed.
     */
    virtual Status _killCursor(OperationContext* opCtx,
                               const NamespaceString& nss,
                               CursorId cursorId) const = 0;
};

}  // namespace mongo
