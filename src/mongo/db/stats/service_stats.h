// service_stats.h

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

#ifndef DB_STATS_SERVICE_STATS_HEADER
#define DB_STATS_SERVICE_STATS_HEADER

#include <string>

#include "../../util/concurrency/spin_lock.h"

namespace mongo {

    using std::string;

    class Histogram;

    /**
     * ServiceStats keeps track of the time a request/response message
     * took inside a service as well as the size of the response
     * generated.
     */
    class ServiceStats {
    public:
        ServiceStats();
        ~ServiceStats();

        /**
         * Record the 'duration' in microseconds a request/response
         * message took and the size in bytes of the generated
         * response.
         */
        void logResponse( uint64_t duration, uint64_t bytes );

        /**
         * Render the histogram as string that can be used inside an
         * HTML doc.
         */
        string toHTML() const;

    private:
        SpinLock   _spinLock;         // protects state below
        Histogram* _timeHistogram;
        Histogram* _spaceHistogram;

        ServiceStats( const ServiceStats& );
        ServiceStats operator=( const ServiceStats& );
    };

}  // namespace mongo

#endif  //  DB_STATS_SERVICE_STATS_HEADER
