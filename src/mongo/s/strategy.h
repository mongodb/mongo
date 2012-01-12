// strategy.h
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

#include "../pch.h"
#include "chunk.h"
#include "request.h"

namespace mongo {

    class Strategy {
    public:
        Strategy() {}
        virtual ~Strategy() {}
        virtual void queryOp( Request& r ) = 0;
        virtual void getMore( Request& r ) = 0;
        virtual void writeOp( int op , Request& r ) = 0;

        void insert( const Shard& shard , const char * ns , const BSONObj& obj , int flags=0 , bool safe=false );

        virtual void commandOp( const string& db, const BSONObj& command, int options,
                                const string& versionedNS, const BSONObj& filter,
                                map<Shard,BSONObj>& results )
        {
            // Only call this from sharded, for now.
            // TODO:  Refactor all this.
            assert( false );
        }

    protected:
        void doWrite( int op , Request& r , const Shard& shard , bool checkVersion = true );
        void doQuery( Request& r , const Shard& shard );

        void insert( const Shard& shard , const char * ns , const vector<BSONObj>& v , int flags=0 , bool safe=false );
        void update( const Shard& shard , const char * ns , const BSONObj& query , const BSONObj& toupdate , int flags=0, bool safe=false );

    };

    extern Strategy * SINGLE;
    extern Strategy * SHARDED;

}

