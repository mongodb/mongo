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
*
*    As a special exception, the copyright holders give permission to link the
*    code of portions of this program with the OpenSSL library under certain
*    conditions as described in each individual source file and distribute
*    linked combinations including the program with the OpenSSL library. You
*    must comply with the GNU Affero General Public License in all respects for
*    all of the code used other than as permitted herein. If you modify file(s)
*    with this exception, you may extend this exception to your version of the
*    file(s), but you are not obligated to do so. If you do not wish to do so,
*    delete this exception statement from your version. If you delete this
*    exception statement from all source files in the program, then also delete
*    it in the license file.
*/

#pragma once

#include "mongo/db/diskloc.h"
#include "mongo/db/index.h"
#include "mongo/db/jsobj.h"
#include "mongo/platform/cstdint.h"

namespace mongo {
    class NamespaceDetails;
    class Record;

    // unindex all keys in index for this record. 
    void unindexRecord(NamespaceDetails *d, Record *todelete, const DiskLoc& dl,
                       bool noWarn = false);

    // Build an index in the foreground
    // If background is false, uses fast index builder
    // If background is true, uses background index builder; blocks until done.
    void buildAnIndex(const std::string& ns,
                      NamespaceDetails *d,
                      IndexDetails& idx,
                      bool mayInterrupt);

    // add index keys for a newly inserted record 
    void indexRecord(const char *ns, NamespaceDetails *d, const BSONObj& obj, const DiskLoc &loc);

    bool dropIndexes(NamespaceDetails *d, const char *ns, const char *name, string &errmsg,
                     BSONObjBuilder &anObjBuilder, bool maydeleteIdIndex );

    /**
     * Add an _id index to namespace @param 'ns' if not already present.
     * @param mayInterrupt When true, killop may interrupt the function call.
     */
    void ensureHaveIdIndex(const char* ns, bool mayInterrupt);

} // namespace mongo
