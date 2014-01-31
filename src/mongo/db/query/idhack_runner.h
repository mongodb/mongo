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

#include <boost/scoped_ptr.hpp>
#include <string>

#include "mongo/base/status.h"
#include "mongo/db/diskloc.h"
#include "mongo/db/query/runner.h"

namespace mongo {

    class BSONObj;
    class CanonicalQuery;
    class Collection;
    class DiskLoc;
    class PlanStage;
    class TypeExplain;
    struct PlanInfo;

    /**
     */
    class IDHackRunner : public Runner {
    public:

        /** Takes ownership of all the arguments -collection. */
        IDHackRunner(const Collection* collection, CanonicalQuery* query);

        IDHackRunner(Collection* collection, const BSONObj& key);

        virtual ~IDHackRunner();

        Runner::RunnerState getNext(BSONObj* objOut, DiskLoc* dlOut);

        virtual bool isEOF();

        virtual void saveState();

        virtual bool restoreState();

        virtual void setYieldPolicy(Runner::YieldPolicy policy);

        virtual void invalidate(const DiskLoc& dl, InvalidationType type);

        virtual const std::string& ns();

        virtual void kill();

        virtual const Collection* collection() { return _collection; }

        virtual Status getInfo(TypeExplain** explain,
                               PlanInfo** planInfo) const;

    private:
        // Not owned here.
        const Collection* _collection;

        // The value to match against the _id field.
        BSONObj _key;

        // TODO: When we combine the canonicalize and getRunner steps into one we can get rid of
        // this.
        boost::scoped_ptr<CanonicalQuery> _query;

        // Are we allowed to release the lock?
        Runner::YieldPolicy _policy;

        // Did someone call kill() on us?
        bool _killed;

        // Have we returned our one document?
        bool _done;

        // If we're yielding to fetch a document, what is it's diskloc?  It may be invalidated
        // while we're yielded.
        DiskLoc _locFetching;

        // Number of index keys scanned: should be either 0 or 1.
        int _nscanned;

        // Number of objects scanned: should be either 0 or 1.
        int _nscannedObjects;
    };

}  // namespace mongo

