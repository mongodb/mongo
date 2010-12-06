// mr.h

/**
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

#include "pch.h"

namespace mongo {
    
    namespace mr {

        class MRReduceState {
        public:

            MRReduceState() : reduce(), finalize(){}

            scoped_ptr<Scope> scope;
            
            ScriptingFunction reduce;
            ScriptingFunction finalize;
        };


        typedef vector<BSONObj> BSONList;

        class MyCmp {
        public:
            MyCmp(){}
            bool operator()( const BSONObj &l, const BSONObj &r ) const {
                return l.firstElement().woCompare( r.firstElement() ) < 0;
            }
        };
        
        typedef map< BSONObj,BSONList,MyCmp > InMemory;

        /**
         * holds map/reduce config information
         */
        class Config {
        public:
            Config( const string& _dbname , const BSONObj& cmdObj , bool markAsTemp = true );

            /** Field expected to be a Code or CodeWScope.
             * Add its scope, if any, to scopeBuilder, and return its code.
             * Scopes added later will shadow those added earlier. */
            static string scopeAndCode (BSONObjBuilder& scopeBuilder, const BSONElement& field);

            /**
               @return number objects in collection
             */
            long long renameIfNeeded( DBDirectClient& db , MRReduceState * state );
            
            string dbname;
            string ns;
            
            // options
            bool verbose;            
            bool keeptemp;
            bool replicate;

            // query options
            
            BSONObj filter;
            BSONObj sort;
            long long limit;

            // functions
            
            string mapCode;
            string reduceCode;
            string finalizeCode;
            
            BSONObj mapparams;
            BSONObj scopeSetup;
            
            // output tables
            string incLong;
            
            string tempShort;
            string tempLong;
            
            string finalShort;
            string finalLong;

            enum { NORMAL , MERGE , REDUCE } outType;
            
            static AtomicUInt JOB_NUMBER;
        }; // end MRsetup
        
        /**
         * container for all stack based map/reduce state
         */
        class MRState : public MRReduceState {
        public:
            MRState( Config& s );
            void init();
            
            void finalReduce( BSONList& values );

            void insert( const string& ns , BSONObj& o );

            Config& setup;
            DBDirectClient db;

            ScriptingFunction map;
        };
        
        /**
         * thread local map/reduce state
         * used for access map/reduce state from inside javascript calls
         */
        class MRTL {
        public:
            MRTL( MRState& state );
            
            void reduceInMemory();

            void dump();
            
            void emit( const BSONObj& a );

            void checkSize();

        private:
            void write( BSONObj& o );
            
            MRState& _state;
        
            boost::shared_ptr<InMemory> _temp;
            long _size;
            
        public:
            long long numEmits;
        };

    } // end mr namespace
}


