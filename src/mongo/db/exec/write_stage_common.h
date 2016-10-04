/**
 *    Copyright (C) 2016 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
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

#include "mongo/platform/basic.h"

#include "mongo/db/exec/working_set.h"

namespace mongo {

class CanonicalQuery;
class Collection;
class OperationContext;

namespace write_stage_common {

/**
 * Returns true if the document referred to by 'id' still exists and matches the query predicate
 * given by 'cq'. Returns true if the document still exists and 'cq' is null. Returns false
 * otherwise.
 *
 * May throw a WriteConflictException if there was a conflict while searching to see if the document
 * still exists.
 */
bool ensureStillMatches(const Collection* collection,
                        OperationContext* txn,
                        WorkingSet* ws,
                        WorkingSetID id,
                        const CanonicalQuery* cq);
}
}
