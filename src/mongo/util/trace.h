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

#pragma once

#include "pch.h"

#include <boost/unordered_map.hpp>
#include "util/concurrency/rwlock.h"


namespace mongo {

    class Trace :
        public boost::noncopyable {
    public:

#ifdef LATER
        /**
           Set the traces with the given names.

           @param name comma separated trace names
        */
        static void setTraces(const string &names);
#endif

        static void setTrace(const string &name, unsigned level);

        /**
           Test to see if the given trace is on or off.

           @param name the name of the trace to check
           @returns true if the trace is on, false otherwise
        */
        static unsigned getTrace(const string &name);

    private:
        Trace();
        ~Trace();

        struct Hash :
        unary_function<string, size_t> {
            size_t operator()(const string &rS) const;
        };

        typedef boost::unordered_map<string, unsigned, Trace::Hash> MapType;
        class NameMap {
        public:
            NameMap();

            MapType traces;
        };

        static NameMap *pMap; /* used by Trace(), so precedes it */
        static SimpleRWLock lock; /* used by Trace(), so precedes it */
        static Trace trace;
    };

}

/* ======================= PRIVATE IMPLEMENTATIONS ========================== */

namespace mongo {

    inline size_t Trace::Hash::operator()(const string &rS) const {
        size_t seed = 0xf0afbeef;
        boost::hash_combine(seed, rS);
        return seed;
    }

}
