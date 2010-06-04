// @file rs_optime.h

/*
 *    Copyright (C) 2010 10gen Inc.
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

namespace mongo {

    struct RSOpTime { 
        unsigned long long ord;

        RSOpTime() : ord(0) { }

        bool initiated() const { return ord > 0; }

        void initiate() { 
            assert( !initiated() );
            ord = 1000000;
        }

        ReplTime inc() {
            DEV assertInWriteLock();
            return ++ord;
        }
    };

    extern RSOpTime rsOpTime;

}
