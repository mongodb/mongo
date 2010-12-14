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

        typedef vector<BSONObj> BSONList;

        class State;

        // ------------  function interfaces -----------

        class Mapper : boost::noncopyable {
        public:
            virtual ~Mapper(){}
            virtual void init( State * state ) = 0;

            virtual void map( const BSONObj& o ) = 0;
        };
        
        class Finalizer : boost::noncopyable {
        public:
            virtual ~Finalizer(){}
            virtual void init( State * state ) = 0;
            
            virtual BSONObj finalize( const BSONObj& o ) = 0;
        };
        
        class Reducer : boost::noncopyable {
        public:
            virtual ~Reducer(){}
            virtual void init( State * state ) = 0;
            
            virtual BSONObj reduce( const BSONList& tuples ) = 0;
            /** this means its a fianl reduce, even if there is no finalizer */
            virtual BSONObj reduce( const BSONList& tuples , Finalizer * finalizer ) = 0;
        };
        
        // ------------  js function implementations -----------        
        
        /**
         * used as a holder for Scope and ScriptingFunction
         * visitor like pattern as Scope is gotten from first access
         */
        class JSFunction : boost::noncopyable {
        public:
            /**
             * @param type (map|reduce|finalzie)
             */
            JSFunction( string type , const BSONElement& e );
            virtual ~JSFunction(){}
            
            virtual void init( State * state );

            Scope * scope(){ return _scope; }
            ScriptingFunction func(){ return _func; }

        private:
            string _type;
            string _code; // actual javascript code
            BSONObj _wantedScope; // this is for CodeWScope 
            
            Scope * _scope; // this is not owned by us, and might be shared
            ScriptingFunction _func;
        };

        class JSMapper : public Mapper {
        public:
            JSMapper( const BSONElement & code ) : _func( "map" , code ){}
            virtual void map( const BSONObj& o );
            virtual void init( State * state );
            
        private:
            JSFunction _func;
            BSONObj _params;
        };
        
        class JSReducer : public Reducer {
        public:
            JSReducer( const BSONElement& code ) : _func( "reduce" , code ){}
            virtual void init( State * state ){ _func.init( state ); }

            virtual BSONObj reduce( const BSONList& tuples );
            virtual BSONObj reduce( const BSONList& tuples , Finalizer * finalizer );

        private:

            /**
             * result in "return"
             * @param key OUT 
             * @param endSizeEstimate OUT
            */
            void _reduce( const BSONList& values , BSONObj& key , int& endSizeEstimate );
            
            JSFunction _func;

        };
        
        class JSFinalizer : public Finalizer  {
        public:
            JSFinalizer( const BSONElement& code ) : _func( "finalize" , code ){}
            virtual BSONObj finalize( const BSONObj& o );
            virtual void init( State * state ){ _func.init( state ); }
        private:
            JSFunction _func;

        };

        // -----------------
        

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
            
            scoped_ptr<Mapper> mapper;
            scoped_ptr<Reducer> reducer;
            scoped_ptr<Finalizer> finalizer;
            
            BSONObj mapparams;
            BSONObj scopeSetup;
            
            // output tables
            string incLong;
            
            string tempShort;
            string tempLong;
            
            string finalShort;
            string finalLong;

            enum { REPLACE , // atomically replace the collection
                   MERGE ,  // merge keys, override dups
                   REDUCE // merge keys, reduce dups
            } outType;
            
            static AtomicUInt JOB_NUMBER;
        }; // end MRsetup
        
        /**
         * stores information about intermediate map reduce state
         * controls flow of data from map->reduce->finalize->output
         */
        class State {
        public:
            State( Config& c );
            void init();

            // ---- map stage ---- 
            
            /**
             * stages on in in-memory storage
             */
            void emit( const BSONObj& a );

            /**
             * if size is big, run a reduce
             * if its still big, dump to temp collection
             */
            void checkSize();

            /**
             * run reduce on _temp
             */
            void reduceInMemory();

            /**
             * transfers in memory storage to temp collection
             */
            void dumpToInc();

            // ------ reduce stage -----------

            void prepTempCollection();            
            
            void finalReduce( BSONList& values );
            
            // ------- cleanup/data positioning ----------

            /**
               @return number objects in collection
             */
            long long renameIfNeeded();

            
            // -------- util ------------
            
            void insert( const string& ns , BSONObj& o );
            
            // ------ simple accessors -----

            /** State maintains ownership, do no use past State lifetime */
            Scope* scope() { return _scope.get(); }
            
            Config& config() { return _config; }

            long long numEmits() const { return _numEmits; }

        private:

            void _insertToInc( BSONObj& o );
            void _emit( const BSONObj& a );

            scoped_ptr<Scope> _scope;
            Config& _config;

            DBDirectClient _db;

            shared_ptr<InMemory> _temp;
            long _size;
            
            long long _numEmits;
        };

        /**
         * keeps all temporary state in memory
         * if data is larger than can fit in a BSONObj to return
         * will throw an exception
         */
        class StateInMemory : public State {

        };
        
        /**
         * keeps some things in memory and pushes
         * to disk when gets too big for ram
         * intended for when output will end up on disk
         */
        class StateOnDisk : public State {
            
        };

        BSONObj fast_emit( const BSONObj& args );

    } // end mr namespace
}


