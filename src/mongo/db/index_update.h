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

#include "mongo/db/diskloc.h"
#include "mongo/db/index.h"
#include "mongo/db/jsobj.h"

namespace mongo {
    class NamespaceDetails;
    class Record;

    // unindex all keys in index for this record. 
    void unindexRecord(NamespaceDetails *d, Record *todelete, const DiskLoc& dl, bool noWarn = false);

    // Build an index in the foreground
    // If background is false, uses fast index builder
    // If background is true, uses background index builder; blocks until done.
    void buildAnIndex(string ns, NamespaceDetails *d, IndexDetails& idx, int idxNo, bool background);

    // add index keys for a newly inserted record 
    // done in two steps/phases to allow potential deferal of write lock portion in the future
    void indexRecordUsingTwoSteps(const char *ns, NamespaceDetails *d, BSONObj obj,
                                         DiskLoc loc, bool shouldBeUnlocked);

    // Given an object, populate "inserter" with information necessary to update indexes.
    void fetchIndexInserters(BSONObjSet & /*out*/keys,
                             IndexInterface::IndexInserter &inserter,
                             NamespaceDetails *d,
                             int idxNo,
                             const BSONObj& obj,
                             DiskLoc recordLoc);
}
