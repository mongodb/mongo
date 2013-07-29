/**
 *    Copyright (C) 2013 10gen Inc.
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

#include <string>

#include "mongo/db/namespace_string.h"
#include "mongo/db/jsobj.h"

namespace mongo {

    /**
     * Merges a chunks in the specified [minKey, maxKey) range of the specified namespace.
     * Updates the local and remote metadata by expanding the bounds of the first chunk in the
     * range, dropping the others, and incrementing the minor version of the shard to the next
     * higher version.  Returns true on success.
     *
     * Fails with errMsg if the 'epoch' was set and has changed (indicating the range is no longer
     * valid), or if the range does not exactly start and stop on chunks owned by this shard, or
     * if the chunks in this range are not contiguous.
     *
     * WARNING: On network failure, it is possible that the chunks in our local metadata may not
     * match the remote metadata, however the key ranges protected will be the same.  All metadata
     * operations are responsible for updating the metadata before performing any actions.
     *
     * Locking note:
     *     + Takes a distributed lock over the namespace
     *     + Cannot be called with any other locks held
     */
    bool mergeChunks( const NamespaceString& nss,
                      const BSONObj& minKey,
                      const BSONObj& maxKey,
                      const OID& epoch,
                      bool onlyMergeEmpty,
                      std::string* errMsg );
}
