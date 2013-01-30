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

#pragma once

#include "mongo/base/string_data.h"
#include "mongo/db/namespacestring.h"

namespace mongo {
namespace authindex {

    /**
     * Ensures that exactly the appropriate indexes are present on system collections supporting
     * authentication and authorization in database "dbname".
     *
     * It is appropriate to call this function on new or existing databases, though it is primarily
     * intended for use on existing databases.  Under no circumstances may it be called on databases
     * with running operations.
     */
    void configureSystemIndexes(const StringData& dbname);

    /**
     * Creates the appropriate indexes on _new_ system collections supporting authentication and
     * authorization.
     */
    void createSystemIndexes(const NamespaceString& ns);

}  // namespace authindex
}  // namespace mongo
