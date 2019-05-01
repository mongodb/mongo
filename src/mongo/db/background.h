/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

/* background.h

   Concurrency coordination for administrative operations.
*/

#pragma once

#include <iosfwd>
#include <map>
#include <set>
#include <vector>

#include "mongo/base/string_data.h"
#include "mongo/db/namespace_string.h"

namespace mongo {

class OperationContext;

/* these are administrative operations / jobs
   for a namespace running in the background, and that if in progress,
   you aren't allowed to do other NamespaceDetails major manipulations
   (such as dropping ns or db) even in the foreground and must
   instead uassert.

   It's assumed this is not for super-high RPS things, so we don't do
   anything special in the implementation here to be fast.
*/
class BackgroundOperation {
    BackgroundOperation(const BackgroundOperation&) = delete;
    BackgroundOperation& operator=(const BackgroundOperation&) = delete;

public:
    static bool inProgForDb(StringData db);
    static int numInProgForDb(StringData db);
    static bool inProgForNs(StringData ns);
    static void assertNoBgOpInProg();
    static void assertNoBgOpInProgForDb(StringData db);
    static void assertNoBgOpInProgForNs(StringData ns);
    static void awaitNoBgOpInProgForDb(StringData db);
    static void awaitNoBgOpInProgForNs(StringData ns);
    static void dump(std::ostream&);

    static bool inProgForNs(const NamespaceString& ns) {
        return inProgForNs(ns.ns());
    }
    static void assertNoBgOpInProgForNs(const NamespaceString& ns) {
        assertNoBgOpInProgForNs(ns.ns());
    }
    static void awaitNoBgOpInProgForNs(const NamespaceString& ns) {
        awaitNoBgOpInProgForNs(ns.ns());
    }

    /**
     * Waits until an index build on collection 'ns' finishes. If there are no index builds in
     * progress, returns immediately.
     *
     * Note: a collection lock should not be held when calling this, as that would block index
     * builds from finishing and this function ever returning.
     */
    static void waitUntilAnIndexBuildFinishes(OperationContext* opCtx, StringData ns);

    /* check for in progress before instantiating */
    BackgroundOperation(StringData ns);

    virtual ~BackgroundOperation();

private:
    NamespaceString _ns;
};

}  // namespace mongo
