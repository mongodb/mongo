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

#include "mongo/db/commands/rename_collection.h"

#include <string>
#include <vector>

#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/namespacestring.h"

namespace mongo {
namespace rename_collection {

    void addPrivilegesRequiredForRenameCollection(const BSONObj& cmdObj,
                                                  std::vector<Privilege>* out) {
        NamespaceString sourceNS = NamespaceString(cmdObj.getStringField("renameCollection"));
        NamespaceString targetNS = NamespaceString(cmdObj.getStringField("to"));
        if (sourceNS.db == targetNS.db) {
            ActionSet actions;
            actions.addAction(ActionType::renameCollectionSameDB);
            out->push_back(Privilege(sourceNS.db, actions));
            return;
        }

        ActionSet sourceActions;
        sourceActions.addAction(ActionType::cloneCollectionLocalSource);
        sourceActions.addAction(ActionType::dropCollection);
        out->push_back(Privilege(sourceNS.ns(), sourceActions));

        ActionSet targetActions;
        targetActions.addAction(ActionType::createCollection);
        targetActions.addAction(ActionType::cloneCollectionTarget);
        targetActions.addAction(ActionType::ensureIndex);
        out->push_back(Privilege(targetNS.ns(), targetActions));
    }

} // namespace rename_collection
} // namespace mongo
