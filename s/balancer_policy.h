// balancer_policy.h

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

#ifndef S_BALANCER_POLICY_HEADER
#define S_BALANCER_POLICY_HEADER

#include "../pch.h"

namespace mongo {

    class BalancerPolicy {
    public:
        struct ChunkInfo;
        typedef shared_ptr<ChunkInfo> ChunkInfoPtr;

        BalancerPolicy() : _balancedLastTime(0){}

        /**
         * Given a connection to the authoritative config server
         * 'conn', fill in 'toBalance' with chunks that could be moved
         * around so to even out storage space across shards.
         */
        void balance( DBClientBase& conn , vector<ChunkInfoPtr>* toBalance );

        /**
         * The previous round of balance influences the next one. We
         * need the chunk moving mechanism to tell us how many chunks
         * effectivel changed home in the last round.
         */
        void setBalancedLastTime( int val ){ _balancedLastTime = val; }

        // below exposed for testing purposes only -- treat it as private --

        void balance( DBClientBase& conn , const string& ns , const BSONObj& data , vector<ChunkInfoPtr>* toBalance );

        BSONObj pickChunk( vector<BSONObj>& from, vector<BSONObj>& to );


    private:
        int _balancedLastTime;
    };

    struct BalancerPolicy::ChunkInfo {
        const string ns;
        const string to;
        const string from;
        const BSONObj chunk;

        ChunkInfo( const string& a_ns , const string& a_to , const string& a_from , const BSONObj& a_chunk )
            : ns( a_ns ) , to( a_to ) , from( a_from ), chunk( a_chunk ){}
    };

}  // namespace mongo

#endif  // S_BALANCER_POLICY_HEADER
