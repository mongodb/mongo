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
        RunnerYieldPolicy() : _elapsedTracker(256, 20) { }

        bool shouldYield() {
            return _elapsedTracker.intervalHasElapsed();
        }

        void yield(Record* rec = NULL) {
            _elapsedTracker.resetLastTime();
            staticYield(rec);
        }

        // TODO: ns is only used in staticYield for an error condition that we may or may not
        // actually care about.
        static void staticYield(Record* rec = NULL) {
            ClientCursor::staticYield(ClientCursor::suggestYieldMicros(), "", rec);
        }

    private:
        ElapsedTracker _elapsedTracker;
    };

} // namespace mongo
