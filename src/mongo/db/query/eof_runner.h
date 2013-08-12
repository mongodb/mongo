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
        EOFRunner(CanonicalQuery* canonicalQuery) : _canonicalQuery(canonicalQuery),
                                                    _killed(false) { }

        Runner::RunnerState getNext(BSONObj* objOut) {
            return Runner::RUNNER_EOF;
        }

        virtual void saveState() { }

        virtual void restoreState() { }

        virtual void invalidate(const DiskLoc& dl) { }

        virtual const CanonicalQuery& getQuery() { return *_canonicalQuery; }

        virtual void kill() { _killed = true; }

        virtual bool forceYield() {
            // Not sure when this would ever happen.
            ClientCursor::registerRunner(this);
            ClientCursor::staticYield(ClientCursor::suggestYieldMicros(),
                                      getQuery().getParsed().ns(),
                                      NULL);
            ClientCursor::deregisterRunner(this);
            return !_killed;
        }

    private:
        scoped_ptr<CanonicalQuery> _canonicalQuery;
        bool _killed;
    };

}  // namespace mongo


