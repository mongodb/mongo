/**
 *    Copyright (C) 2013 MongoDB Inc.
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

namespace mongo {

class Collection;
class OperationContext;
class UpdateIndexData;


class UpdateLifecycle {
public:
    virtual ~UpdateLifecycle() {}

    /**
     * Update the cached collection pointer that this lifecycle object uses.
     */
    virtual void setCollection(Collection* collection) = 0;

    /**
     * Can the update continue?
     *
     * The (only) implementation will check the following:
     *  1.) Collection still exists
     *  2.) Shard version has not changed (indicating that the query/update is not valid
     */
    virtual bool canContinue() const = 0;

    /**
     * Return a pointer to any indexes if there is a collection.
     */
    virtual const UpdateIndexData* getIndexKeys(OperationContext* opCtx) const = 0;
};

}  // namespace mongo
