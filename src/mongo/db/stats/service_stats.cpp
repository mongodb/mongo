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
