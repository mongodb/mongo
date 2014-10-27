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

#include "mongo/db/catalog/collection.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/util/elapsed_tracker.h"

namespace mongo {

    class PlanYieldPolicy {
    public:
        explicit PlanYieldPolicy(PlanExecutor* exec);

        /**
         * Used by YIELD_AUTO plan executors in order to check whether it is time to yield.
         * PlanExecutors give up their locks periodically in order to be fair to other
         * threads.
         */
        bool shouldYield();

        /**
         * Used to cause a plan executor to give up locks and go to sleep. The PlanExecutor
         * must *not* be in saved state. Handles calls to save/restore state internally.
         *
         * By default, assumes that the PlanExecutor is already registered. If 'registerPlan'
         * is explicitly set to true, then the executor will get automatically registered and
         * deregistered here.
         *
         * Returns true if the executor was restored successfully and is still alive. Returns false
         * if the executor got killed during yield.
         */
        bool yield(bool registerPlan = false);

    private:
        // Default constructor disallowed in order to ensure initialization of '_planYielding'.
        PlanYieldPolicy();

        ElapsedTracker _elapsedTracker;

        // The plan executor which this yield policy is responsible for yielding. Must
        // not outlive the plan executor.
        PlanExecutor* _planYielding;
    };

} // namespace mongo

