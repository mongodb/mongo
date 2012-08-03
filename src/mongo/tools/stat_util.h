// stat_util.h

/**
 *    Copyright (C) 2008 10gen Inc.
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

#include "mongo/pch.h"
#include "../db/jsobj.h"

namespace mongo {


    struct NamespaceInfo {
        string ns;
        
        // these need to be in millis
        long long read;
        long long write;
        
        string toString() const {
            stringstream ss;
            ss << ns << " r: " << read << " w: " << write;
            return ss.str();
        }

    };

    struct NamespaceDiff {
        string ns;
        
        long long read;
        long long write;
        
        NamespaceDiff( NamespaceInfo prev , NamespaceInfo now ) {
            ns = prev.ns;
            read = now.read - prev.read;
            write = now.write - prev.write;
        }
        
        long long total() const { return read + write; }
        
        bool operator<(const NamespaceDiff& r) const {
            return total() < r.total();
        }
    };

    typedef map<string,NamespaceInfo> NamespaceStats;

    /**
     * static methods useful for computing status from serverStatus type things
     */
    class StatUtil {
    public:
        /**
         * @param seconds - seconds between calls to serverStatus
         * @param all - show all fields
         */
        StatUtil( double seconds = 1 , bool all = false );

        /**
         * @param a older serverStatus
         * @param b newer serverStatus
         */
        BSONObj doRow( const BSONObj& a , const BSONObj& b );

        double getSeconds() const { return _seconds; }
        bool getAll() const { return _all; }

        void setSeconds( double seconds ) { _seconds = seconds; }
        void setAll( bool all ) { _all = all; }

        static NamespaceStats parseServerStatusLocks( const BSONObj& serverStatus );
        static vector<NamespaceDiff> computeDiff( const NamespaceStats& prev , const NamespaceStats& current );
    private:


        double percent( const char * outof , const char * val , const BSONObj& a , const BSONObj& b );
        
        double diff( const string& name , const BSONObj& a , const BSONObj& b );

        void _appendMem( BSONObjBuilder& result , const string& name , unsigned width , double sz );

        void _appendNet( BSONObjBuilder& result , const string& name , double diff );

        template<typename T>
        void _append( BSONObjBuilder& result , const string& name , unsigned width , const T& t ) {
            if ( name.size() > width )
                width = name.size();
            result.append( name , BSON( "width" << (int)width << "data" << t ) );
        }

        bool _in( const BSONElement& me , const BSONElement& arr );

        
        // -------

        double _seconds;
        bool _all;
        
    };
}

