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
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

/**
 * Useful utilities for clients working on a cluster.  Safe wrapping of operations useful in
 * general for clients using cluster metadata.
 *
 * TODO: See if this stuff is more generally useful, distribute if so.
 */

#pragma once

#include "mongo/base/owned_pointer_map.h"
#include "mongo/base/owned_pointer_vector.h"
#include "mongo/client/dbclientinterface.h"
#include "mongo/s/type_chunk.h"
#include "mongo/s/type_collection.h"

namespace mongo {

    //
    // Helper methods for querying information about a cluster
    //

    /**
     * Tries to check the versions of all active hosts in a cluster.  Not 100% accurate, but pretty
     * effective if hosts are reachable.
     *
     * Returns OK if hosts are compatible as far as we know, RemoteValidationError if hosts are not
     * compatible, and an error Status if anything else goes wrong.
     */
    Status checkClusterMongoVersions(const ConnectionString& configLoc,
                                     const std::string& minMongoVersion);

    /**
     * Logs to the config.changelog collection
     *
     * Returns OK if loaded successfully, error Status if not.
     */
    Status logConfigChange(const ConnectionString& configLoc,
                           const std::string& clientHost,
                           const std::string& ns,
                           const std::string& description,
                           const BSONObj& details);

    //
    // Needed to normalize exception behavior of connections and cursors
    // TODO: Remove when we refactor the client connection interface to something more consistent.
    //

    // Helper function which throws for invalid cursor initialization.
    // Note: cursor ownership will be passed to this function.
    DBClientCursor* _safeCursor(std::auto_ptr<DBClientCursor> cursor);

}
