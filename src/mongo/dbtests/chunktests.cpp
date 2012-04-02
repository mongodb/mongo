//@file chunktests.cpp : s/chunk.{h,cpp} tests

/**
 *    Copyright (C) 2012 10gen Inc.
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

#include "pch.h"

#include "../s/chunk.h"
#include "mongo/db/json.h"

#include "dbtests.h"

namespace mongo {

    class TestableChunkManager : public ChunkManager {
    public:
        void setShardKey( const BSONObj &keyPattern ) {
            const_cast<ShardKeyPattern&>(_key) = ShardKeyPattern( keyPattern );
        }
        void setSingleChunkForShards( const vector<BSONObj> &splitPoints ) {
            ChunkMap &chunkMap = const_cast<ChunkMap&>( _chunkMap );
            ChunkRangeManager &chunkRanges = const_cast<ChunkRangeManager&>( _chunkRanges );
            set<Shard> &shards = const_cast<set<Shard>&>( _shards );
            
            vector<BSONObj> mySplitPoints( splitPoints );
            mySplitPoints.insert( mySplitPoints.begin(), _key.globalMin() );
            mySplitPoints.push_back( _key.globalMax() );
            
            for( unsigned i = 1; i < mySplitPoints.size(); ++i ) {
                string name = str::stream() << (i-1);
                Shard shard( name, name );
                shards.insert( shard );
                
                ChunkPtr chunk( new Chunk( this, mySplitPoints[ i-1 ], mySplitPoints[ i ],
                                          shard ) );
                chunkMap[ mySplitPoints[ i ] ] = chunk;
            }
            
            chunkRanges.reloadAll( chunkMap );
        }
    };
    
} // namespace mongo

namespace ChunkTests {

    namespace ChunkManagerTests {
        
        typedef mongo::TestableChunkManager ChunkManager;
        
        class Create {
        public:
            void run() {
                ChunkManager chunkManager;
            }
        };
        
        class Base {
        public:
            virtual ~Base() {}
            void run() {
                ChunkManager chunkManager;
                chunkManager.setShardKey( shardKey() );
                chunkManager.setSingleChunkForShards( splitPointsVector() );
                
                set<Shard> shards;
                chunkManager.getShardsForQuery( shards, query() );
                
                BSONArrayBuilder b;
                for( set<Shard>::const_iterator i = shards.begin(); i != shards.end(); ++i ) {
                    b << i->getName();
                }
                ASSERT_EQUALS( expectedShardNames(), b.arr() );
            }
        protected:
            virtual BSONObj shardKey() const { return BSON( "a" << 1 ); }
            virtual BSONArray splitPoints() const { return BSONArray(); }
            virtual BSONObj query() const { return BSONObj(); }
            virtual BSONArray expectedShardNames() const { return BSON_ARRAY( "0" ); }
            virtual vector<BSONObj> splitPointsVector() const {
                vector<BSONObj> ret;
                BSONArray a = splitPoints();
                BSONObjIterator i( a );
                while( i.more() ) {
                    ret.push_back( i.next().Obj().getOwned() );
                }
                return ret;
            }
        };
        
        class EmptyQuerySingleShard : public Base {
        };
        
        class MultiShardBase : public Base {
            virtual BSONArray splitPoints() const {
                return BSON_ARRAY( BSON( "a" << "x" ) << BSON( "a" << "y" ) << BSON( "a" << "z" ) );
            }            
        };
        
        class EmptyQueryMultiShard : public MultiShardBase {
            virtual BSONArray expectedShardNames() const {
                return BSON_ARRAY( "0" << "1" << "2" << "3" );
            }
        };

        class UniversalRangeMultiShard : public EmptyQueryMultiShard {
            virtual BSONObj query() const { return BSON( "b" << 1 ); }
        };
        
        class EqualityRangeSingleShard : public EmptyQuerySingleShard {
            virtual BSONObj query() const { return BSON( "a" << "x" ); }
        };

        class EqualityRangeMultiShard : public MultiShardBase {
            virtual BSONObj query() const { return BSON( "a" << "y" ); }
            virtual BSONArray expectedShardNames() const { return BSON_ARRAY( "2" ); }
        };
        
        class SetRangeMultiShard : public MultiShardBase {
            virtual BSONObj query() const { return fromjson( "{a:{$in:['u','y']}}" ); }
            virtual BSONArray expectedShardNames() const { return BSON_ARRAY( "0" << "2" ); }
        };
        
        class GTRangeMultiShard : public MultiShardBase {
            virtual BSONObj query() const { return BSON( "a" << GT << "x" ); }
            virtual BSONArray expectedShardNames() const { return BSON_ARRAY( "1" << "2" << "3" ); }
        };

        class GTERangeMultiShard : public MultiShardBase {
            virtual BSONObj query() const { return BSON( "a" << GTE << "x" ); }
            virtual BSONArray expectedShardNames() const { return BSON_ARRAY( "1" << "2" << "3" ); }
        };

        class LTRangeMultiShard : public MultiShardBase {
            virtual BSONObj query() const { return BSON( "a" << LT << "y" ); }
            /**
             * It isn't actually necessary to return shard 2 because its lowest key is "y", which
             * is excluded from the query.  SERVER-4791
             */
            virtual BSONArray expectedShardNames() const { return BSON_ARRAY( "0" << "1" << "2" ); }
        };

        class LTERangeMultiShard : public MultiShardBase {
            virtual BSONObj query() const { return BSON( "a" << LTE << "y" ); }
            virtual BSONArray expectedShardNames() const { return BSON_ARRAY( "0" << "1" << "2" ); }
        };

        class OrEqualities : public MultiShardBase {
            virtual BSONObj query() const { return fromjson( "{$or:[{a:'u'},{a:'y'}]}" ); }
            virtual BSONArray expectedShardNames() const { return BSON_ARRAY( "0" << "2" ); }
        };
        
        class OrEqualityInequality : public MultiShardBase {
            virtual BSONObj query() const { return fromjson( "{$or:[{a:'u'},{a:{$gte:'y'}}]}" ); }
            virtual BSONArray expectedShardNames() const { return BSON_ARRAY( "0" << "2" << "3" ); }
        };

        class OrEqualityInequalityUnhelpful : public MultiShardBase {
            virtual BSONObj query() const {
                return fromjson( "{$or:[{a:'u'},{a:{$gte:'zz'}},{}]}" );
            }
            virtual BSONArray expectedShardNames() const {
                return BSON_ARRAY( "0" << "1" << "2" << "3" );
            }
        };

        template<class BASE>
        class Unsatisfiable : public BASE {
            /**
             * SERVER-4914 For now the first shard is returned for unsatisfiable queries, as some
             * clients of getShardsForQuery() expect at least one shard.
             */
            virtual BSONArray expectedShardNames() const {
                return BSON_ARRAY( "0" ) /* BSONArray() */;
            }
        };
        
        class UnsatisfiableRangeSingleShard : public Unsatisfiable<Base> {
            virtual BSONObj query() const { return BSON( "a" << GT << "x" << LT << "x" ); }
        };
        
        class UnsatisfiableRangeMultiShard : public Unsatisfiable<MultiShardBase> {
            virtual BSONObj query() const { return BSON( "a" << GT << "x" << LT << "x" ); }            
        };

        class EqualityThenUnsatisfiable : public Unsatisfiable<Base> {
            virtual BSONObj shardKey() const { return BSON( "a" << 1 << "b" << 1 ); }
            virtual BSONObj query() const { return BSON( "a" << 1 << "b" << GT << 4 << LT << 4 ); }
        };

        class InequalityThenUnsatisfiable : public Unsatisfiable<Base> {
            virtual BSONObj shardKey() const { return BSON( "a" << 1 << "b" << 1 ); }
            virtual BSONObj query() const {
                return BSON( "a" << GT << 1 << "b" << GT << 4 << LT << 4 );
            }
        };
        
        class OrEqualityUnsatisfiableInequality : public MultiShardBase {
            virtual BSONObj query() const {
                return fromjson( "{$or:[{a:'x'},{a:{$gt:'u',$lt:'u'}},{a:{$gte:'y'}}]}" );
            }
            virtual BSONArray expectedShardNames() const { return BSON_ARRAY( "1" << "2" << "3" ); }
        };

        class CompoundKeyBase : public Base {
            virtual BSONObj shardKey() const {
                return BSON( "a" << 1 << "b" << 1 );
            }
            virtual BSONArray splitPoints() const {
                return BSON_ARRAY( BSON( "a" << 5 << "b" << 10 ) << BSON ( "a" << 5 << "b" << 20 ) );
            }
        };

        class InMultiShard : public CompoundKeyBase {
            virtual BSONObj query() const {
                return BSON( "a" << BSON( "$in" << BSON_ARRAY( 0 << 5 << 10 ) ) <<
                             "b" << BSON( "$in" << BSON_ARRAY( 0 << 5 << 25 ) ) );
            }
            // If we were to send this query to just the shards it actually needed to hit, it would only hit shards 0 and 2
            // Because of the optimization from SERVER-4745, however, we'll also hit shard 1.
            virtual BSONArray expectedShardNames() const {
                return BSON_ARRAY( "0" << "1" << "2" );
            }
        };

    } // namespace ChunkManagerTests
    
    class All : public Suite {
    public:
        All() : Suite( "chunk" ) {
        }
        
        void setupTests() {
            add<ChunkManagerTests::Create>();
            add<ChunkManagerTests::EmptyQuerySingleShard>();
            add<ChunkManagerTests::EmptyQueryMultiShard>();
            add<ChunkManagerTests::UniversalRangeMultiShard>();
            add<ChunkManagerTests::EqualityRangeSingleShard>();
            add<ChunkManagerTests::EqualityRangeMultiShard>();
            add<ChunkManagerTests::SetRangeMultiShard>();
            add<ChunkManagerTests::GTRangeMultiShard>();
            add<ChunkManagerTests::GTERangeMultiShard>();
            add<ChunkManagerTests::LTRangeMultiShard>();
            add<ChunkManagerTests::LTERangeMultiShard>();
            add<ChunkManagerTests::OrEqualities>();
            add<ChunkManagerTests::OrEqualityInequality>();
            add<ChunkManagerTests::OrEqualityInequalityUnhelpful>();
            add<ChunkManagerTests::UnsatisfiableRangeSingleShard>();
            add<ChunkManagerTests::UnsatisfiableRangeMultiShard>();
            add<ChunkManagerTests::EqualityThenUnsatisfiable>();
            add<ChunkManagerTests::InequalityThenUnsatisfiable>();
            add<ChunkManagerTests::OrEqualityUnsatisfiableInequality>();
            add<ChunkManagerTests::InMultiShard>();
        }
    } myall;
    
} // namespace ChunkTests
