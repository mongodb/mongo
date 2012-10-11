/**
*    Copyright (C) 2012 10gen Inc.
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
*/

#include "mongo/pch.h"

#include "mongo/db/auth/action_type.h"

#include <string>

#include "mongo/base/status.h"
#include "mongo/platform/cstdint.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

    const ActionType ActionType::READ(READ_VALUE);
    const ActionType ActionType::READ_WRITE(READ_WRITE_VALUE);
    const ActionType ActionType::USER_ADMIN(USER_ADMIN_VALUE);
    const ActionType ActionType::DB_ADMIN(DB_ADMIN_VALUE);
    const ActionType ActionType::SERVER_ADMIN(SERVER_ADMIN_VALUE);
    const ActionType ActionType::CLUSTER_ADMIN(CLUSTER_ADMIN_VALUE);

    bool ActionType::operator==(const ActionType& rhs) const {
        return _identifier == rhs._identifier;
    }

    std::ostream& operator<<(std::ostream& os, const ActionType& at) {
        os << ActionType::actionToString(at);
        return os;
    }

    Status ActionType::parseActionFromString(const std::string& action, ActionType* result) {
        if (action == "r") {
            *result = READ;
            return Status::OK();
        } else if (action == "w") {
            *result = READ_WRITE;
            return Status::OK();
        } else if (action == "u") {
            *result = USER_ADMIN;
            return Status::OK();
        } else if (action == "d") {
            *result = DB_ADMIN;
            return Status::OK();
        } else if (action == "s") {
            *result = SERVER_ADMIN;
            return Status::OK();
        } else if (action == "c") {
            *result = CLUSTER_ADMIN;
            return Status::OK();
        } else {
            return Status(ErrorCodes::FailedToParse,
                          mongoutils::str::stream() << "Unrecognized action capability string: "
                                                    << action,
                          0);
        }
    }

    // Takes an ActionType and returns the string representation
    std::string ActionType::actionToString(const ActionType& action) {
        switch (action.getIdentifier()) {
        case READ_VALUE:
            return "r";
        case READ_WRITE_VALUE:
            return "w";
        case USER_ADMIN_VALUE:
            return "u";
        case DB_ADMIN_VALUE:
            return "d";
        case SERVER_ADMIN_VALUE:
            return "s";
        case CLUSTER_ADMIN_VALUE:
            return "c";
        default:
            return "";
        }
    }

} // namespace mongo
