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

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/db/catalog/collection_options.h"

namespace mongo {
class BSONObj;
class BSONObjBuilder;
class Collection;
class NamespaceString;
class OperationContext;

void updateUUIDSchemaVersion(OperationContext* opCtx, bool upgrade);

/**
 * Performs the collection modification described in "cmdObj" on the collection "ns".
 */
Status collMod(OperationContext* opCtx,
               const NamespaceString& ns,
               const BSONObj& cmdObj,
               BSONObjBuilder* result);

/*
 * Adds uuid to the collection "ns" if uuid exists and removes any existing UUID from
 * the collection "ns" if uuid is boost::none. This is called in circumstances where
 * we may be upgrading or downgrading and we need to update the UUID.
 */
Status collModForUUIDUpgrade(OperationContext* opCtx,
                             const NamespaceString& ns,
                             const BSONObj& cmdObj,
                             OptionalCollectionUUID uuid);
}  // namespace mongo
