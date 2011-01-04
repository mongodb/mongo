// service_stats.cpp

/**
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

#include <sstream>

#include "../../util/histogram.h"
#include "service_stats.h"

namespace mongo {

    using std::ostringstream;

    ServiceStats::ServiceStats() {
        // Time histogram covers up to 128msec in exponential intervals
        // starting at 125usec.
        Histogram::Options timeOpts;
        timeOpts.numBuckets = 12;
        timeOpts.bucketSize = 125;
        timeOpts.exponential = true;
        _timeHistogram = new Histogram( timeOpts );

        // Space histogram covers up to 1MB in exponentialintervals starting
        // at 1K.
        Histogram::Options spaceOpts;
        spaceOpts.numBuckets = 12;
        spaceOpts.bucketSize = 1024;
        spaceOpts.exponential = true;
        _spaceHistogram = new Histogram( spaceOpts );
    }

    ServiceStats::~ServiceStats() {
        delete _timeHistogram;
        delete _spaceHistogram;
    }

    void ServiceStats::logResponse( uint64_t duration, uint64_t bytes ) {
        _spinLock.lock();
        _timeHistogram->insert( duration / 1000 /* in usecs */ );
        _spaceHistogram->insert( bytes );
        _spinLock.unlock();
    }

    string ServiceStats::toHTML() const {
        ostringstream res ;
        res << "Cumulative wire stats\n"
            << "Response times\n" << _timeHistogram->toHTML()
            << "Response sizes\n" << _spaceHistogram->toHTML()
            << '\n';

        return res.str();
    }

}  // mongo
