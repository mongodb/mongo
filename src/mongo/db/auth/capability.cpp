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

#include "mongo/db/auth/capability.h"

#include <string>
#include <vector>

#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/principal.h"
#include "mongo/platform/cstdint.h"

namespace mongo {

    Capability::Capability(const std::string& resource,
                           Principal* principal,
                           ActionSet actions) :
            _resource(resource),
            _principal(principal),
            _actions(actions) {}


    const Principal* Capability::getPrincipal() const {
        return _principal;
    }

    const std::string& Capability::getResource() const {
        return _resource;
    }

    const ActionSet& Capability::getActions() const {
        return _actions;
    }

    bool Capability::includesAction(const ActionType& action) const {
        return _actions.contains(action);
    }

} // namespace mongo
