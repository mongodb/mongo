// find_and_modify.cpp

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

#include "mongo/db/commands/find_and_modify.h"

#include <string>
#include <vector>

#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/jsobj.h"

namespace mongo {
namespace find_and_modify {

        void addPrivilegesRequiredForFindAndModify(const std::string& dbname,
                                                   const BSONObj& cmdObj,
                                                   std::vector<Privilege>* out) {
            bool update = cmdObj["update"].trueValue();
            bool upsert = cmdObj["upsert"].trueValue();
            bool remove = cmdObj["remove"].trueValue();

            ActionSet actions;
            actions.addAction(ActionType::find);
            if (update) {
                actions.addAction(ActionType::update);
            }
            if (upsert) {
                actions.addAction(ActionType::insert);
            }
            if (remove) {
                actions.addAction(ActionType::remove);
            }
            std::string ns = dbname + '.' + cmdObj.firstElement().valuestr();
            out->push_back(Privilege(ns, actions));
        }

} // namespace find_and_modify
} // namespace mongo
