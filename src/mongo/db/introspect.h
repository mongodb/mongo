// introspect.h
// system management stuff.

/**
*    Copyright (C) 2008 10gen Inc.
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

#include "mongo/pch.h"
#include "jsobj.h"
#include "pdfile.h"

namespace mongo {

    /* --- profiling --------------------------------------------
       do when database->profile is set
    */

    void profile(const Client& c, int op, CurOp& currentOp);

    /**
     * Get (or create) the profile collection
     *
     * @param   db      Database in which to create the profile collection
     * @param   force   Always create the collection if it does not exist
     * @return  NamespaceDetails for the newly created collection, or NULL on error
    **/
    NamespaceDetails* getOrCreateProfileCollection(Database *db, bool force = false, string* errmsg = NULL);

} // namespace mongo
