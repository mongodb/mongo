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
        EOFRunner(CanonicalQuery* cq, const string& ns) : _cq(cq), _ns(ns) { }

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

        virtual void kill() { }

    private:
        scoped_ptr<CanonicalQuery> _cq;
        string _ns;
    };

}  // namespace mongo
