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
#include "mongo/util/elapsed_tracker.h"

namespace mongo {

    class RunnerYieldPolicy {
    public:
        RunnerYieldPolicy() : _elapsedTracker(128, 10) { }

        bool shouldYield() {
            return _elapsedTracker.intervalHasElapsed();
        }

        void yield(Record* rec = NULL) {
            if (staticYield(rec)) {
                _elapsedTracker.resetLastTime();
            }
        }

        static bool staticYield(Record* rec = NULL) {
            int micros = ClientCursor::suggestYieldMicros();

            if (micros > 0) {
                ClientCursor::staticYield(micros, "", rec);
                return true;
            }

            return false;
        }

    private:
        ElapsedTracker _elapsedTracker;
    };

} // namespace mongo
