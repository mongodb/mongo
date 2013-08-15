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
 */

#pragma once

#include "mongo/db/clientcursor.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/lite_parsed_query.h"
#include "mongo/db/query/plan_cache.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/runner.h"
#include "mongo/db/query/stage_builder.h"

namespace mongo {

    /**
     * EOFRunner is EOF immediately and doesn't do anything except return EOF and possibly die
     * during a yield.
     */
    class EOFRunner : public Runner {
    public:
        EOFRunner(const string& ns) : _ns(ns), _cq(CanonicalQuery::getInternalQuery()) { }

        Runner::RunnerState getNext(BSONObj* objOut, DiskLoc* dlOut) {
            return Runner::RUNNER_EOF;
        }

        virtual bool isEOF() { return true; }

        virtual void saveState() { }

        virtual bool restoreState() {
            // TODO: Does this value matter?
            return false;
        }

        virtual void setYieldPolicy(Runner::YieldPolicy policy) { }

        virtual void invalidate(const DiskLoc& dl) { }

        virtual const string& ns() { return _ns; }

        virtual const CanonicalQuery& getQuery() {
            // This shouldn't be called and is going away shortly anyway.
            verify(0);
            return *_cq;
        }

        virtual void kill() { }

    private:
        string _ns;
        scoped_ptr<CanonicalQuery> _cq;
    };

}  // namespace mongo


