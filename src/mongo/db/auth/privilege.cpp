/*    Copyright 2012 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#include "mongo/db/auth/privilege.h"

#include <string>

#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"

namespace mongo {

    Privilege::Privilege(const std::string& resource, const ActionType& action) :
        _resource(resource) {

        _actions.addAction(action);
    }
    Privilege::Privilege(const std::string& resource, const ActionSet& actions) :
            _resource(resource), _actions(actions) {}

    void Privilege::addActions(const ActionSet& actionsToAdd) {
        _actions.addAllActionsFromSet(actionsToAdd);
    }

    void Privilege::removeActions(const ActionSet& actionsToRemove) {
        _actions.removeAllActionsFromSet(actionsToRemove);
    }

    bool Privilege::includesAction(const ActionType& action) const {
        return _actions.contains(action);
    }

    bool Privilege::includesActions(const ActionSet& actions) const {
        return _actions.isSupersetOf(actions);
    }

} // namespace mongo
