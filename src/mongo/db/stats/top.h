// top.h : DB usage monitor.

/*    Copyright 2009 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#pragma once

#include <boost/date_time/posix_time/posix_time.hpp>

namespace mongo {

    /**
     * tracks usage by collection
     */
    class Top {

    public:
        Top() : _lock("Top") { }

        struct UsageData {
            UsageData() : time(0) , count(0) {}
            UsageData( const UsageData& older , const UsageData& newer );
            long long time;
            long long count;

            void inc( long long micros ) {
                count++;
                time += micros;
            }
        };

        struct CollectionData {
            /**
             * constructs a diff
             */
            CollectionData() {}
            CollectionData( const CollectionData& older , const CollectionData& newer );

            UsageData total;

            UsageData readLock;
            UsageData writeLock;

            UsageData queries;
            UsageData getmore;
            UsageData insert;
            UsageData update;
            UsageData remove;
            UsageData commands;
        };

        typedef map<string,CollectionData> UsageMap;

    public:
        void record( const string& ns , int op , int lockType , long long micros , bool command );
        void append( BSONObjBuilder& b );
        void cloneMap(UsageMap& out) const;
        CollectionData getGlobalData() const { return _global; }
        void collectionDropped( const string& ns );

    public: // static stuff
        static Top global;

    private:
        void _appendToUsageMap( BSONObjBuilder& b , const UsageMap& map ) const;
        void _appendStatsEntry( BSONObjBuilder& b , const char * statsName , const UsageData& map ) const;
        void _record( CollectionData& c , int op , int lockType , long long micros , bool command );

        mutable mongo::mutex _lock;
        CollectionData _global;
        UsageMap _usage;
        string _lastDropped;
    };

} // namespace mongo
