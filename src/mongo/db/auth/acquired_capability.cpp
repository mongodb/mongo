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

#include "mongo/db/auth/acquired_capability.h"

#include "mongo/db/auth/capability.h"
#include "mongo/db/auth/principal.h"

namespace mongo {

    AcquiredCapability::AcquiredCapability(const Capability& capability,
                                           Principal* principal) :
            _capability(capability),
            _principal(principal) {}

    AcquiredCapability::AcquiredCapability(const std::string& resource,
                                           Principal* principal,
                                           ActionSet actions) :
            _capability(resource, actions),
            _principal(principal) {}


    const Principal* AcquiredCapability::getPrincipal() const {
        return _principal;
    }

    const std::string& AcquiredCapability::getResource() const {
        return _capability.getResource();
    }

    const ActionSet& AcquiredCapability::getActions() const {
        return _capability.getActions();
    }

    bool AcquiredCapability::includesAction(const ActionType& action) const {
        return _capability.includesAction(action);
    }

} // namespace mongo
