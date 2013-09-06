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
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#pragma once

#include "mongo/pch.h"

#include <boost/unordered_map.hpp>
#include "mongo/util/concurrency/rwlock.h"


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
