// @file pagefault.h

/**
*    Copyright (C) 2012 10gen Inc.
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

    class Record;

    class PageFaultException /*: public DBException*/ { 
        unsigned era;
        const Record *r;
    public:
        PageFaultException(const PageFaultException& rhs) : era(rhs.era), r(rhs.r) { }
        explicit PageFaultException(const Record*);
        void touch();
    };

    class PageFaultRetryableSection : boost::noncopyable { 
        unsigned _laps;
    public:
        unsigned laps() const { return _laps; }
        void didLap() { _laps++; }
        PageFaultRetryableSection();
        ~PageFaultRetryableSection();
    };

    /**
     * this turns off page faults in a scope
     * there are just certain arease where its dangerous
     * this might mean the code is dangerous anyway....
     */
    class NoPageFaultsAllowed : boost::noncopyable {
    public:
        NoPageFaultsAllowed();
        ~NoPageFaultsAllowed();
    private:
        PageFaultRetryableSection* _saved;
    };

#if 0
    inline void how_to_use_example() {
        // ...
        {
            PageFaultRetryableSection s;
            while( 1 ) {
                try {
                    writelock lk; // or readlock
                    // do work
                    break;
                }
                catch( PageFaultException& e ) { 
                    e.touch();
                } 
            }
        }
        // ...
    }
#endif
}
