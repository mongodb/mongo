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
#include "mongo/db/catalog/collection.h"
#include "mongo/util/elapsed_tracker.h"

namespace mongo {

    class RunnerYieldPolicy {
    public:
        RunnerYieldPolicy() : _elapsedTracker(128, 10), _runnerYielding(NULL) { }

        ~RunnerYieldPolicy() {
            if (NULL != _runnerYielding) {
                // We were destructed mid-yield.  Since we're being used to yield a runner, we have
                // to deregister the runner.
                if ( _runnerYielding->collection() ) {
                    _runnerYielding->collection()->cursorCache()->deregisterRunner(_runnerYielding);
                }
            }
        }

        bool shouldYield() {
            return _elapsedTracker.intervalHasElapsed();
        }

        /**
         * Yield the provided runner, registering and deregistering it appropriately.  Deal with
         * deletion during a yield by setting _runnerYielding to ensure deregistration.
         *
         * Provided runner MUST be YIELD_MANUAL.
         */
        bool yieldAndCheckIfOK(Runner* runner) {
            invariant(runner);
            invariant(runner->collection());

            int micros = ClientCursor::suggestYieldMicros();

            // If micros is not positive, no point in yielding, nobody waiting.
            //
            // TODO: Track how many times we actually yield, how many times micros is <0, etc.
            if (micros <= 0) { return true; }

            // If micros > 0, we should yield.
            runner->saveState();
            _runnerYielding = runner;

            runner->collection()->cursorCache()->registerRunner( _runnerYielding );

            // Note that this call checks for interrupt, and thus can throw if interrupt flag is set
            staticYield(micros);

            if ( runner->collection() ) {
                // if the runner was killed, runner->collection() will return NULL
                // so we don't deregister as it was done when killed
                runner->collection()->cursorCache()->deregisterRunner( _runnerYielding );
            }
            _runnerYielding = NULL;
            _elapsedTracker.resetLastTime();
            return runner->restoreState();
        }

        /**
         * Yield.  Caller is in charge of all runner registration.
         *
         * Used for YIELD_AUTO runners.
         */
        void yield() {
            int micros = ClientCursor::suggestYieldMicros();

            if (micros > 0) {
                staticYield(micros);
                // TODO:  When do we really want to reset this?  Currently we reset it when we
                // actually yield.  As such we'll keep on trying to yield once the tracker has
                // elapsed.  If we reset it even if we don't yield, we'll wait until the time
                // interval elapses again to try yielding.
                _elapsedTracker.resetLastTime();
            }
        }

        static void staticYield(int micros) {
            ClientCursor::staticYield(micros, "");
        }

    private:
        ElapsedTracker _elapsedTracker;
        Runner* _runnerYielding;
    };

} // namespace mongo
