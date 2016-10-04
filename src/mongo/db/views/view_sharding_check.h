/**
 *    Copyright (C) 2016 MongoDB Inc.
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

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"

#include <string>
#include <vector>

namespace mongo {

class Database;
class NamespaceString;
class OperationContext;
class ViewDefinition;

/**
 * When a read against a view is forwarded from mongoS, it is done so without any awareness as to
 * whether the underlying collection is sharded. If it is found that the underlying collection is
 * sharded(*) we return an error to mongos with the view definition requesting
 * that the resolved read operation be executed there.
 *
 * (*) We have incomplete sharding state on secondaries. If we are a secondary, then we have to
 * assume that the collection backing the view could be sharded.
 */
class ViewShardingCheck {
public:
    /**
     * If it is determined that a view's underlying collection may be sharded this method returns
     * a BSONObj containing the resolved view definition. If the underlying collection is not
     * sharded an empty BSONObj is returned.
     *
     * Will return an error if the ViewCatalog is unable to generate a resolved view.
     */
    static StatusWith<BSONObj> getResolvedViewIfSharded(OperationContext* opCtx,
                                                        Database* db,
                                                        const ViewDefinition* view);

    /**
     * Appends the resolved view definition and CommandOnShardedViewNotSupportedOnMongod status to
     * 'result'.
     */
    static void appendShardedViewStatus(const BSONObj& resolvedView, BSONObjBuilder* result);

private:
    /**
     * Confirms whether 'ns' represents a sharded collection. Only valid if the calling
     * member is primary.
     */
    static bool collectionIsSharded(OperationContext* opCtx, const NamespaceString& nss);
};

}  // namespace mongo
