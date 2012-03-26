/**
 * Copyright (c) 2012 10gen Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "util/trace.h"

namespace mongo {

    /* the singleton static instance of this object */
    Trace::NameMap *Trace::pMap = NULL;
    SimpleRWLock Trace::lock("Trace");
    Trace Trace::trace;

    Trace::NameMap::NameMap():
        traces() {
    }

    Trace::Trace() {
        /*
          This static singleton is constructed at program load time, so the
          lock should not be necessary here.
         */
        Trace::pMap = new NameMap();
    }

    Trace::~Trace() {
        delete Trace::pMap;
    }

    void Trace::setTrace(const string &name, unsigned level) {
        SimpleRWLock::Exclusive xlock(Trace::lock); // dtor unlocks

        /* if the new level is to be zero, we're going to remove the entry */
        if (level == 0) {
            Trace::pMap->traces.erase(name);
            return;
        }

        /* try to insert the new trace */
        std::pair<MapType::iterator, bool> i(
            Trace::pMap->traces.insert(
                MapType::value_type(name, level)));

        /*
          If the insert didn't take place, there was already an entry for
          that name.  Set it to have the new level.
         */
        if (!i.second) {
            (*i.first).second = level;
        }
    }

#ifdef LATER
    void Trace::setTraces(const string &names) {
        /* create a new map, and replace the existing one */
        NameMap *pM;
        verify(false);
    }
#endif

    unsigned Trace::getTrace(const string &name) {
        SimpleRWLock::Shared slock(Trace::lock); // dtor unlocks

        /* quickest check for no active traces */
        if (Trace::pMap->traces.empty())
            return 0;

        /* there are traces, so look up by name */
        MapType::const_iterator i(Trace::pMap->traces.find(name));
        if (i == Trace::pMap->traces.end())
            return 0;

        return (*i).second;
    }
}
