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
#include "util/intrusive_counter.h"


namespace mongo {

    class DependencyTracker :
        public IntrusiveCounterUnsigned {
    public:
        void include(const string &fieldName);
        void exclude(const string &fieldName);

        bool isRequired(const string &fieldName) const;

    private:
        struct Tracker {
            string fieldName;

            struct Hash :
                unary_function<string, size_t> {
                size_t operator()(const string &rS) const;
            };
        };

        boost::unordered_map<string, Tracker, Tracker::Hash> map;
    };

}

/* ======================= INLINED IMPLEMENTATIONS ========================== */

namespace mongo {

    inline size_t DependencyTracker::Tracker::Hash::operator()(
        const string &rS) const {
        size_t seed = 0xf0afbeef;
        boost::hash_combine(seed, rS);
        return seed;
    }

}
