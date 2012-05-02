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
            virtual ~Mapper() {}
            virtual void init( State * state ) = 0;

            virtual void map( const BSONObj& o ) = 0;
        };

        class Finalizer : boost::noncopyable {
        public:
            virtual ~Finalizer() {}
            virtual void init( State * state ) = 0;

            /**
             * this takes a tuple and returns a tuple
             */
            virtual BSONObj finalize( const BSONObj& tuple ) = 0;
        };

        class Reducer : boost::noncopyable {
        public:
            Reducer() : numReduces(0) {}
            virtual ~Reducer() {}
            virtual void init( State * state ) = 0;

            virtual BSONObj reduce( const BSONList& tuples ) = 0;
            /** this means its a final reduce, even if there is no finalizer */
            virtual BSONObj finalReduce( const BSONList& tuples , Finalizer * finalizer ) = 0;

            long long numReduces;
        };

        // ------------  js function implementations -----------

        /**
         * used as a holder for Scope and ScriptingFunction
         * visitor like pattern as Scope is gotten from first access
         */
        class JSFunction : boost::noncopyable {
        public:
            /**
             * @param type (map|reduce|finalize)
             */
            JSFunction( string type , const BSONElement& e );
            virtual ~JSFunction() {}

            virtual void init( State * state );

            Scope * scope() const { return _scope; }
            ScriptingFunction func() const { return _func; }

        private:
            string _type;
            string _code; // actual javascript code
            BSONObj _wantedScope; // this is for CodeWScope

            Scope * _scope; // this is not owned by us, and might be shared
            ScriptingFunction _func;
        };

        class JSMapper : public Mapper {
        public:
            JSMapper( const BSONElement & code ) : _func( "_map" , code ) {}
            virtual void map( const BSONObj& o );
            virtual void init( State * state );

        private:
            JSFunction _func;
            BSONObj _params;
        };

        class JSReducer : public Reducer {
        public:
            JSReducer( const BSONElement& code ) : _func( "_reduce" , code ) {}
            virtual void init( State * state );

            virtual BSONObj reduce( const BSONList& tuples );
            virtual BSONObj finalReduce( const BSONList& tuples , Finalizer * finalizer );

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
            JSFinalizer( const BSONElement& code ) : _func( "_finalize" , code ) {}
            virtual BSONObj finalize( const BSONObj& o );
            virtual void init( State * state ) { _func.init( state ); }
        private:
            JSFunction _func;

        };

        // -----------------


        class TupleKeyCmp {
        public:
            TupleKeyCmp() {}
            bool operator()( const BSONObj &l, const BSONObj &r ) const {
                return l.firstElement().woCompare( r.firstElement() ) < 0;
            }
        };

        typedef map< BSONObj,BSONList,TupleKeyCmp > InMemory; // from key to list of tuples

        /**
         * holds map/reduce config information
         */
        class Config {
        public:
            Config( const string& _dbname , const BSONObj& cmdObj );

            string dbname;
            string ns;

            // options
            bool verbose;
            bool jsMode;
            int splitInfo;

            // query options

            BSONObj filter;
            BSONObj sort;
            long long limit;

            // functions

            scoped_ptr<Mapper> mapper;
            scoped_ptr<Reducer> reducer;
            scoped_ptr<Finalizer> finalizer;

            BSONObj mapParams;
            BSONObj scopeSetup;

            // output tables
            string incLong;
            string tempLong;

            string finalShort;
            string finalLong;

            string outDB;

            // max number of keys allowed in JS map before switching mode
            long jsMaxKeys;
            // ratio of duplicates vs unique keys before reduce is triggered in js mode
            float reduceTriggerRatio;
            // maximum size of map before it gets dumped to disk
            long maxInMemSize;

            enum { REPLACE , // atomically replace the collection
                   MERGE ,  // merge keys, override dups
                   REDUCE , // merge keys, reduce dups
                   INMEMORY // only store in memory, limited in size
                 } outType;

            // if true, no lock during output operation
            bool outNonAtomic;

            // true when called from mongos to do phase-1 of M/R
            bool shardedFirstPass;

            static AtomicUInt JOB_NUMBER;
        }; // end MRsetup

        /**
         * stores information about intermediate map reduce state
         * controls flow of data from map->reduce->finalize->output
         */
        class State {
        public:
            State( const Config& c );
            ~State();

            void init();

            // ---- prep  -----
            bool sourceExists();

            long long incomingDocuments();

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
            void insertToInc( BSONObj& o );
            void _insertToInc( BSONObj& o );

            // ------ reduce stage -----------

            void prepTempCollection();

            void finalReduce( BSONList& values );

            void finalReduce( CurOp * op , ProgressMeterHolder& pm );

            // ------- cleanup/data positioning ----------

            /**
               @return number objects in collection
             */
            long long postProcessCollection( CurOp* op , ProgressMeterHolder& pm );
            long long postProcessCollectionNonAtomic( CurOp* op , ProgressMeterHolder& pm );

            /**
             * if INMEMORY will append
             * may also append stats or anything else it likes
             */
            void appendResults( BSONObjBuilder& b );

            // -------- util ------------

            /**
             * inserts with correct replication semantics
             */
            void insert( const string& ns , const BSONObj& o );

            // ------ simple accessors -----

            /** State maintains ownership, do no use past State lifetime */
            Scope* scope() { return _scope.get(); }

            const Config& config() { return _config; }

            const bool isOnDisk() { return _onDisk; }

            long long numEmits() const { if (_jsMode) return _scope->getNumberLongLong("_emitCt"); return _numEmits; }
            long long numReduces() const { if (_jsMode) return _scope->getNumberLongLong("_redCt"); return _config.reducer->numReduces; }
            long long numInMemKeys() const { if (_jsMode) return _scope->getNumberLongLong("_keyCt"); return _temp->size(); }

            bool jsMode() {return _jsMode;}
            void switchMode(bool jsMode);
            void bailFromJS();

            const Config& _config;
            DBDirectClient _db;

        protected:

            void _add( InMemory* im , const BSONObj& a , long& size );

            scoped_ptr<Scope> _scope;
            bool _onDisk; // if the end result of this map reduce is disk or not

            scoped_ptr<InMemory> _temp;
            long _size; // bytes in _temp
            long _dupCount; // number of duplicate key entries

            long long _numEmits;

            bool _jsMode;
            ScriptingFunction _reduceAll;
            ScriptingFunction _reduceAndEmit;
            ScriptingFunction _reduceAndFinalize;
            ScriptingFunction _reduceAndFinalizeAndInsert;
        };

        BSONObj fast_emit( const BSONObj& args, void* data );
        BSONObj _bailFromJS( const BSONObj& args, void* data );

    } // end mr namespace
}


