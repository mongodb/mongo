/**
 *    Copyright (C) 2015 MongoDB Inc.
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

#include <string>

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"

namespace mongo {
class BSONObj;
class OperationContext;
class BSONElement;

/**
 * Creates a collection as described in "cmdObj" on the database "dbName". Creates the collection's
 * _id index according to 'idIndex', if it is non-empty. When 'idIndex' is empty, creates the
 * default _id index.
 */
Status createCollection(OperationContext* opCtx,
                        const std::string& dbName,
                        const BSONObj& cmdObj,
                        const BSONObj& idIndex = BSONObj());

/**
 * As above, but only used by replication to apply operations. This allows recreating collections
 * with specific UUIDs (if ui is given), and in that case will rename any existing collections with
 * the same name and a UUID to a temporary name. If ui is not given, an existing collection will
 * result in an error.
 */
Status createCollectionForApplyOps(OperationContext* opCtx,
                                   const std::string& dbName,
                                   const BSONElement& ui,
                                   const BSONObj& cmdObj,
                                   const BSONObj& idIndex = BSONObj());

}  // namespace mongo
