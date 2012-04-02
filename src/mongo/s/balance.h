//@file balance.h

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

#include "../pch.h"
#include "../util/background.h"
#include "balancer_policy.h"

namespace mongo {

    /**
     * The balancer is a background task that tries to keep the number of chunks across all servers of the cluster even. Although
     * every mongos will have one balancer running, only one of them will be active at the any given point in time. The balancer
     * uses a 'DistributedLock' for that coordination.
     *
     * The balancer does act continuously but in "rounds". At a given round, it would decide if there is an imbalance by
     * checking the difference in chunks between the most and least loaded shards. It would issue a request for a chunk
     * migration per round, if it found so.
     */
    class Balancer : public BackgroundJob {
    public:
        Balancer();
        virtual ~Balancer();

        // BackgroundJob methods

        virtual void run();

        virtual string name() const { return "Balancer"; }

    private:
        typedef BalancerPolicy::ChunkInfo CandidateChunk;
        typedef shared_ptr<CandidateChunk> CandidateChunkPtr;

        // hostname:port of my mongos
        string _myid;

        // time the Balancer started running
        time_t _started;

        // number of moved chunks in last round
        int _balancedLastTime;

        // decide which chunks to move; owned here.
        scoped_ptr<BalancerPolicy> _policy;
        
        /**
         * Checks that the balancer can connect to all servers it needs to do its job.
         *
         * @return true if balancing can be started
         *
         * This method throws on a network exception
         */
        bool _init();

        /**
         * Gathers all the necessary information about shards and chunks, and decides whether there are candidate chunks to
         * be moved.
         *
         * @param conn is the connection with the config server(s)
         * @param candidateChunks (IN/OUT) filled with candidate chunks, one per collection, that could possibly be moved
         */
        void _doBalanceRound( DBClientBase& conn, vector<CandidateChunkPtr>* candidateChunks );

        /**
         * Issues chunk migration request, one at a time.
         *
         * @param candidateChunks possible chunks to move
         * @return number of chunks effectively moved
         */
        int _moveChunks( const vector<CandidateChunkPtr>* candidateChunks );

        /**
         * Marks this balancer as being live on the config server(s).
         *
         * @param conn is the connection with the config server(s)
         */
        void _ping( DBClientBase& conn, bool waiting = false );

        /**
         * @return true if all the servers listed in configdb as being shards are reachable and are distinct processes
         */
        bool _checkOIDs();

    };

    extern Balancer balancer;
}
