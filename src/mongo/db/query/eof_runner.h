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

// THIS FILE IS DEPRECATED -- Runner to be replaced with PlanExecutor.

#pragma once

#include <boost/scoped_ptr.hpp>
#include <string>

#include "mongo/base/status.h"
#include "mongo/db/query/runner.h"

namespace mongo {

    class BSONObj;
    class CanonicalQuery;
    class DiskLoc;
    class TypeExplain;
    struct PlanInfo;

    /**
     * EOFRunner is EOF immediately and doesn't do anything except return EOF and possibly die
     * during a yield.
     */
    class EOFRunner : public Runner {
    public:

        /* Takes ownership */
        EOFRunner(CanonicalQuery* cq, const std::string& ns);

        virtual ~EOFRunner();

        virtual Runner::RunnerState getNext(BSONObj* objOut, DiskLoc* dlOut);

        virtual bool isEOF();

        virtual void saveState();

        virtual bool restoreState(OperationContext* opCtx);

        virtual void invalidate(const DiskLoc& dl, InvalidationType type);

        virtual const std::string& ns();

        virtual void kill();

        // this can return NULL since we never yield or anything over it
        virtual const Collection* collection() { return NULL; }

        /**
         * Always returns OK, allocating and filling in '*explain' with a fake ("zeroed")
         * collection scan plan. Fills in '*planInfo' with information indicating an
         * EOF runner. Caller owns '*explain', though.
         */
        virtual Status getInfo(TypeExplain** explain,
                               PlanInfo** planInfo) const;

    private:
        boost::scoped_ptr<CanonicalQuery> _cq;
        std::string _ns;
    };

}  // namespace mongo
