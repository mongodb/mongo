// btreeperf.cpp

/*    Copyright 2010 10gen Inc.
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

/**
 * Performance timing and space utilization testing for btree indexes.
 */

#include <iostream>

#include <boost/random/bernoulli_distribution.hpp>
#include <boost/random/geometric_distribution.hpp>
#include <boost/random/mersenne_twister.hpp>
#include <boost/random/variate_generator.hpp>
#include <boost/random/uniform_int.hpp>

#include "client/dbclient.h"
#include "../../util/timer.h"

using namespace std;
using namespace mongo;
using namespace boost;

const char *ns = "test.btreeperf";
const char *db = "test";
const char *index_collection = "btreeperf.$_id_";

// This random number generator has a much larger period than the default
// generator and is half as fast as the default.  Given that we intend to
// generate large numbers of documents and will utilize more than one random
// sample per document, choosing this generator seems like a worthwhile tradeoff.
mt19937 randomNumberGenerator;

/**
 * An interface for generating documents to be inserted and document specs for
 * remove requests.
 */
class InsertAndRemoveStrategy {
public:
    virtual ~InsertAndRemoveStrategy() {}
    virtual BSONObj insertObj() = 0;
    virtual BSONObj removeObj() = 0;
protected:
    /**
     * Helper functions for converting a sample value to a sample object with
     * specified _id, to be inserted or removed.
     */

    template< class T >
    BSONObj insertObjWithVal( const T &val ) {
        BSONObjBuilder b;
        b.append( "_id", val );
        return b.obj();
    }
    template< class T >
    BSONObj removeObjWithVal( const T &val ) {
        BSONObjBuilder b;
        b.append( "_id", val );
        return b.obj();
    }
};

/**
 * Manages a set of elements of type T.  Supports inserting unique elements and
 * sampling a random element without replacement.
 *
 * TODO In the contexts where this class is currently used, duplicate keys are
 * either impossible or highly unlikely.  And an occasional duplicate value will
 * not much affect the procedure by which a random element is chosen.  We could
 * stop checking for duplicates in push(), eliminate _set from the implementation,
 * and potentially improve performance and memory requirements somewhat.
 */
template< class T >
class SetSampler {
public:
    /** @param val Insert this value in the set if not already present. */
    void push( const T& val ) {
        if ( _set.insert( val ).second ) {
            _vector.push_back( val );
        }
    }
    /** @return a random element removed from the set */
    T pull() {
        if ( _vector.size() == 0 ) {
            return T();
        }
        uniform_int< size_t > sizeRange( 0, _vector.size() - 1 );
        variate_generator< mt19937&, uniform_int< size_t > > sizeGenerator( randomNumberGenerator, sizeRange );
        size_t toRemove = sizeGenerator();
        T val = _vector[ toRemove ];
        // Replace the random element with the last element, then remove the
        // last element.
        _vector[ toRemove ] = _vector.back();
        _vector.pop_back();
        _set.erase( val );
        return val;
    }
private:
    vector< T > _vector;
    set< T > _set;
};

/**
 * Tracks values that have been specified for insertion by the derived class's
 * implementation of insertVal() and selects uniformally from among values that
 * have been inserted but not yet removed for the next value to remove.
 *
 * The implementation is probabilistically sound, but may be resource intensive
 * and slow due to the use of a SetSampler.
 */
template< class T >
class InsertAndUniformRemoveStrategy : public InsertAndRemoveStrategy {
public:
    virtual BSONObj insertObj() {
        T val = insertVal();
        _sampler.push( val );
        return insertObjWithVal( val );
    }
    virtual BSONObj removeObj() { return removeObjWithVal( _sampler.pull() ); }
protected:
    /** @return value to insert. This is the only function a derived class need implement. */
    virtual T insertVal() = 0;
private:
    SetSampler< T > _sampler;
};

/**
 * The derived class supplies keys to be inserted and removed.  The key removal
 * strategy is similar to the strategy for selecting a random element described
 * in the MongoDB cookbook: the first key in the collection greater than or
 * equal to the supplied removal key is removed.  This allows selecting an
 * exising key for removal without the overhead required by a SetSampler.
 *
 * While this ranged selection strategy can work well for selecting a random
 * element, there are some theoretical and empirically observed shortcomings
 * when the strategy is applied to removing nodes for btree performance measurement:
 * 1 The likelihood that a given key is removed is proportional to the difference
 *   in value between it and the previous key.  Because key deletion increases
 *   the difference in value between adjacent keys, neighboring keys will be
 *   more likely to be deleted than they would be in a true uniform distribution.
 * 2 MongoDB 1.6 uses 'unused' nodes in the btree implementation.  With a ranged
 *   removal strategy, those nodes must be traversed to find a node available
 *   for removal.
 * 3 Ranged removal was observed to be biased against the balancing policy of
 *   MongoDB 1.7 in some cases, in terms of storage size.  This may be a
 *   consequence of point 1 above.
 * 4 Ranged removal was observed to be significantly biased against the btree
 *   implementation in MongoDB 1.6 in terms of performance.  This is likely a
 *   consequence of point 2 above.
 * 5 In some cases the biases described above were not evident in tests lasting
 *   several minutes, but were evident in tests lasting several hours.
 */
template< class T >
class InsertAndRangedRemoveStrategy : public InsertAndRemoveStrategy {
public:
    virtual BSONObj insertObj() { return insertObjWithVal( insertVal() ); }
    virtual BSONObj removeObj() { return rangedRemoveObjWithVal( removeVal() ); }
protected:
    /** Small likelihood that this removal spec will not match any document */
    template< class U >
    BSONObj rangedRemoveObjWithVal( const U &val ) {
        BSONObjBuilder b1;
        BSONObjBuilder b2( b1.subobjStart( "_id" ) );
        b2.append( "$gte", val );
        b2.done();
        return b1.obj();
    }
    virtual T insertVal() = 0;
    virtual T removeVal() = 0;
};

/**
 * Integer Keys
 * Uniform Inserts
 * Uniform Removes
 */
class UniformInsertRangedUniformRemoveInteger : public InsertAndRangedRemoveStrategy< long long > {
public:
    UniformInsertRangedUniformRemoveInteger() :
        _uniform_int( 0ULL, ~0ULL ),
        _nextLongLong( randomNumberGenerator, _uniform_int ) {
    }
    /** Small likelihood of duplicates */
    virtual long long insertVal() { return _nextLongLong(); }
    virtual long long removeVal() { return _nextLongLong(); }
private:
    uniform_int< unsigned long long > _uniform_int;
    variate_generator< mt19937&, uniform_int< unsigned long long > > _nextLongLong;
};

class UniformInsertUniformRemoveInteger : public InsertAndUniformRemoveStrategy< long long > {
public:
    virtual long long insertVal() { return _gen.insertVal(); }
private:
    UniformInsertRangedUniformRemoveInteger _gen;
};

/**
 * String Keys
 * Uniform Inserts
 * Uniform Removes
 */
class UniformInsertRangedUniformRemoveString : public InsertAndRangedRemoveStrategy< string > {
public:
    UniformInsertRangedUniformRemoveString() :
        _geometric_distribution( 0.9 ),
        _nextLength( randomNumberGenerator, _geometric_distribution ),
        _uniform_char( 'a', 'z' ),
        _nextChar( randomNumberGenerator, _uniform_char ) {
    }
    /** Small likelihood of duplicates */
    virtual string insertVal() { return nextString(); }
    virtual string removeVal() { return nextString(); }
private:
    string nextString() {
        // The longer the minimum string length, the lower the likelihood of duplicates
        int len = _nextLength() + 5;
        len = len > 100 ? 100 : len;
        string ret( len, 'x' );
        for( int i = 0; i < len; ++i ) {
            ret[ i ] = _nextChar();
        }
        return ret;
    }
    geometric_distribution<> _geometric_distribution;
    variate_generator< mt19937&, geometric_distribution<> > _nextLength;
    uniform_int< char > _uniform_char;
    variate_generator< mt19937&, uniform_int< char > > _nextChar;
};

class UniformInsertUniformRemoveString : public InsertAndUniformRemoveStrategy< string > {
public:
    virtual string insertVal() { return _gen.insertVal(); }
private:
    UniformInsertRangedUniformRemoveString _gen;
};

/**
 * OID Keys
 * Increasing Inserts
 * Uniform Removes
 */
class IncreasingInsertRangedUniformRemoveOID : public InsertAndRangedRemoveStrategy< OID > {
public:
    IncreasingInsertRangedUniformRemoveOID() :
        _max( -1 ) {
    }
    virtual OID insertVal() { return oidFromULL( ++_max ); }
    virtual OID removeVal() {
        uniform_int< unsigned long long > distribution( 0, _max > 0 ? _max : 0 );
        variate_generator< mt19937&, uniform_int< unsigned long long > > generator( randomNumberGenerator, distribution );
        return oidFromULL( generator() );
    }
private:
    static OID oidFromULL( unsigned long long val ) {
        val = __builtin_bswap64( val );
        OID oid;
        oid.clear();
        memcpy( (char*)&oid + 4, &val, 8 );
        return oid;
    }
    long long _max;
};

class IncreasingInsertUniformRemoveOID : public InsertAndUniformRemoveStrategy< OID > {
public:
    virtual OID insertVal() { return _gen.insertVal(); }
private:
    IncreasingInsertRangedUniformRemoveOID _gen;
};

/**
 * Integer Keys
 * Increasing Inserts
 * Increasing Removes (on remove, the lowest key is always removed)
 */
class IncreasingInsertIncreasingRemoveInteger : public InsertAndRemoveStrategy {
public:
    IncreasingInsertIncreasingRemoveInteger() :
        // Start with a large value so data type will be preserved if we round
        // trip through json.
        _min( 1LL << 32 ),
        _max( 1LL << 32 ) {
    }
    virtual BSONObj insertObj() { return insertObjWithVal( ++_max ); }
    virtual BSONObj removeObj() { return removeObjWithVal( _min < _max ? ++_min : _min ); }
private:
    long long _min;
    long long _max;
};

/** Generate a random boolean value. */
class BernoulliGenerator {
public:
    /**
     * @param excessFalsePercent This specifies the desired rate of false values
     * vs true values.  If we want false to be 5% more likely than true, we
     * specify 5 for this argument.
     */
    BernoulliGenerator( int excessFalsePercent ) :
        _bernoulli_distribution( 1.0 / ( 2.0 + excessFalsePercent / 100.0 ) ),
        _generator( randomNumberGenerator, _bernoulli_distribution ) {
    }
    bool operator()() { return _generator(); }
private:
    bernoulli_distribution<> _bernoulli_distribution;
    variate_generator< mt19937&, bernoulli_distribution<> > _generator;
};

/** Runs a strategy on a connection, with specified mix of inserts and removes. */
class InsertAndRemoveRunner {
public:
    InsertAndRemoveRunner( DBClientConnection &conn, InsertAndRemoveStrategy &strategy, int excessInsertPercent ) :
        _conn( conn ),
        _strategy( strategy ),
        _nextOpTypeRemove( excessInsertPercent ) {
    }
    void writeOne() {
        if ( _nextOpTypeRemove() ) {
            _conn.remove( ns, _strategy.removeObj(), true );
        }
        else {
            _conn.insert( ns, _strategy.insertObj() );
        }
    }
private:
    DBClientConnection &_conn;
    InsertAndRemoveStrategy &_strategy;
    BernoulliGenerator _nextOpTypeRemove;
};

/**
 * Writes a test script to cout based on a strategy and specified mix of inserts
 * and removes.  The script can be subsequently executed by InsertAndRemoveRunner.
 * Script generation is intended for strategies that are memory or cpu intensive
 * and might either divert resources from a mongod instance being analyzed on the
 * same machine or fail to generate requests as quickly as the mongod might
 * accept them.
 * The script contains one line per operation.  Each line begins
 * with a letter indicating the operation type, followed by a space.  Next
 * follows the json representation of a document for the specified operation
 * type.
 */
class InsertAndRemoveScriptGenerator {
public:
    InsertAndRemoveScriptGenerator( InsertAndRemoveStrategy &strategy, int excessInsertPercent ) :
        _strategy( strategy ),
        _nextOpTypeRemove( excessInsertPercent ) {
    }
    void writeOne() {
        if ( _nextOpTypeRemove() ) {
            cout << "r " << _strategy.removeObj().jsonString() << endl;
        }
        else {
            cout << "i " << _strategy.insertObj().jsonString() << endl;
        }
    }
private:
    InsertAndRemoveStrategy &_strategy;
    BernoulliGenerator _nextOpTypeRemove;
};

/**
 * Run a test script from cin that was generated by
 * InsertAndRemoveScriptGenerator.  Running the script is intended to be
 * lightweight in terms of memory and cpu usage, and fast.
 */
class InsertAndRemoveScriptRunner {
public:
    InsertAndRemoveScriptRunner( DBClientConnection &conn ) :
        _conn( conn ) {
    }
    void writeOne() {
        cin.getline( _buf, 1024 );
        BSONObj val = fromjson( _buf + 2 );
        if ( _buf[ 0 ] == 'r' ) {
            _conn.remove( ns, val, true );
        }
        else {
            _conn.insert( ns, val );
        }
    }
private:
    DBClientConnection &_conn;
    char _buf[ 1024 ];
};

int main( int argc, const char **argv ) {

    DBClientConnection conn;
    conn.connect( "127.0.0.1:27017" );
    conn.dropCollection( ns );

//    UniformInsertRangedUniformRemoveInteger strategy;
//    UniformInsertUniformRemoveInteger strategy;
//    UniformInsertRangedUniformRemoveString strategy;
//    UniformInsertUniformRemoveString strategy;
//    IncreasingInsertRangedUniformRemoveOID strategy;
//    IncreasingInsertUniformRemoveOID strategy;
//    IncreasingInsertIncreasingRemoveInteger strategy;
//    InsertAndRemoveScriptGenerator runner( strategy, 5 );
    InsertAndRemoveScriptRunner runner( conn );

    Timer t;
    BSONObj statsCmd = BSON( "collstats" << index_collection );

    // Print header, unless we are generating a script (in that case, comment this out).
    cout << "ops,milliseconds,docs,totalBucketSize" << endl;

    long long i = 0;
    long long n = 10000000000;
    while( i < n ) {
        runner.writeOne();
        // Print statistics, unless we are generating a script (in that case, comment this out).
        // The stats collection requests below provide regular read operations,
        // ensuring we are caught up with the progress being made by the mongod
        // under analysis.
        if ( ++i % 50000 == 0 ) {
            // The total number of documents present.
            long long docs = conn.count( ns );
            BSONObj result;
            conn.runCommand( db, statsCmd, result );
            // The total number of bytes used for all allocated 8K buckets of the
            // btree.
            long long totalBucketSize = result.getField( "count" ).numberLong() * 8192;
            cout << i << ',' << t.millis() << ',' << docs << ',' << totalBucketSize << endl;
        }
    }
}
