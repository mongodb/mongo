// dbtests.h : Test suite generator headers.
//

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
*    must comply with the GNU Affero General Public License in all respects
*    for all of the code used other than as permitted herein. If you modify
*    file(s) with this exception, you may extend this exception to your
*    version of the file(s), but you are not obligated to do so. If you do not
*    wish to do so, delete this exception statement from your version. If you
*    delete this exception statement from all source files in the program,
*    then also delete it in the license file.
*/

#pragma once


#include "mongo/db/instance.h"
#include "mongo/unittest/unittest.h"

using namespace mongo;
using namespace mongo::unittest;
using std::shared_ptr;

namespace mongo {
class BSONObj;
class OperationContext;
class Status;
class StringData;

namespace dbtests {

/**
 * Creates an index if it does not already exist.
 */
Status createIndex(OperationContext* txn, StringData ns, const BSONObj& keys, bool unique = false);

/**
 * Creates an index from a BSON spec, if it does not already exist.
 */
Status createIndexFromSpec(OperationContext* txn, StringData ns, const BSONObj& spec);
}  // namespace dbtests
}  // namespace mongo
