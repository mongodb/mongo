//@file extsorttests.cpp : mongo/db/extsort.{h,cpp} tests

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

#include <boost/thread.hpp>

#include "mongo/db/extsort.h"
#include "mongo/db/index/btree_based_builder.h"
#include "mongo/db/pdfile.h"
#include "mongo/db/sorter/sorter.h"
#include "mongo/dbtests/dbtests.h"
#include "mongo/platform/cstdint.h"

// Need access to internal classes
#include "mongo/db/sorter/sorter.cpp"

namespace ExtSortTests {
    using boost::make_shared;
    using namespace sorter;

    bool isSolaris() {
#ifdef __sunos__
        return true;
#else
        return false;
#endif
    }

    static const char* const _ns = "unittests.extsort";
    DBDirectClient _client;
    ExternalSortComparison* _arbitrarySort = BtreeBasedBuilder::getComparison(time(0)%2, BSONObj());
    ExternalSortComparison* _aFirstSort = BtreeBasedBuilder::getComparison(0, BSON("a" << 1));

    /** Sort four values. */
    class SortFour {
    public:
        void run() {
            BSONObjExternalSorter sorter( _arbitrarySort );

            sorter.add( BSON( "x" << 10 ), DiskLoc( 5, 1 ), false );
            sorter.add( BSON( "x" << 2 ), DiskLoc( 3, 1 ), false );
            sorter.add( BSON( "x" << 5 ), DiskLoc( 6, 1 ), false );
            sorter.add( BSON( "x" << 5 ), DiskLoc( 7, 1 ), false );

            sorter.sort( false );

            auto_ptr<BSONObjExternalSorter::Iterator> i = sorter.iterator();
            int num=0;
            while ( i->more() ) {
                pair<BSONObj,DiskLoc> p = i->next();
                if ( num == 0 )
                    ASSERT_EQUALS( 2, p.first["x"].number() );
                else if ( num <= 2 ) {
                    ASSERT_EQUALS( 5, p.first["x"].number() );
                }
                else if ( num == 3 )
                    ASSERT_EQUALS( 10, p.first["x"].number() );
                else
                    ASSERT( false );
                num++;
            }

            ASSERT_EQUALS( 0 , sorter.numFiles() );
        }
    };

    /** Sort four values and check disk locs. */
    class SortFourCheckDiskLoc {
    public:
        void run() {
            BSONObjExternalSorter sorter( _arbitrarySort, 10 );
            sorter.add( BSON( "x" << 10 ), DiskLoc( 5, 11 ), false );
            sorter.add( BSON( "x" << 2 ), DiskLoc( 3, 1 ), false );
            sorter.add( BSON( "x" << 5 ), DiskLoc( 6, 1 ), false );
            sorter.add( BSON( "x" << 5 ), DiskLoc( 7, 1 ), false );

            sorter.sort( false );

            auto_ptr<BSONObjExternalSorter::Iterator> i = sorter.iterator();
            int num=0;
            while ( i->more() ) {
                pair<BSONObj,DiskLoc> p = i->next();
                if ( num == 0 ) {
                    ASSERT_EQUALS( 2, p.first["x"].number() );
                    ASSERT_EQUALS( "3:1", p.second.toString() );
                }
                else if ( num <= 2 )
                    ASSERT_EQUALS( 5, p.first["x"].number() );
                else if ( num == 3 ) {
                    ASSERT_EQUALS( 10, p.first["x"].number() );
                    ASSERT_EQUALS( "5:b", p.second.toString() );
                }
                else
                    ASSERT( false );
                num++;
            }
        }
    };

    /** Sort no values. */
    class SortNone {
    public:
        void run() {
            BSONObjExternalSorter sorter( _arbitrarySort, 10 );
            sorter.sort( false );

            auto_ptr<BSONObjExternalSorter::Iterator> i = sorter.iterator();
            ASSERT( ! i->more() );
        }
    };

    /** Check sorting by disk location. */
    class SortByDiskLock {
    public:
        void run() {
            BSONObjExternalSorter sorter( _arbitrarySort );
            sorter.add( BSON( "x" << 10 ), DiskLoc( 5, 4 ), false );
            sorter.add( BSON( "x" << 2 ), DiskLoc( 3, 0 ), false );
            sorter.add( BSON( "x" << 5 ), DiskLoc( 6, 2 ), false );
            sorter.add( BSON( "x" << 5 ), DiskLoc( 7, 3 ), false );
            sorter.add( BSON( "x" << 5 ), DiskLoc( 2, 1 ), false );

            sorter.sort( false );

            auto_ptr<BSONObjExternalSorter::Iterator> i = sorter.iterator();
            int num=0;
            while ( i->more() ) {
                pair<BSONObj,DiskLoc> p = i->next();
                if ( num == 0 )
                    ASSERT_EQUALS( 2, p.first["x"].number() );
                else if ( num <= 3 ) {
                    ASSERT_EQUALS( 5, p.first["x"].number() );
                }
                else if ( num == 4 )
                    ASSERT_EQUALS( 10, p.first["x"].number() );
                else
                    ASSERT( false );
                ASSERT_EQUALS( num , p.second.getOfs() );
                num++;
            }
        }
    };

    /** Sort 1e4 values. */
    class Sort1e4 {
    public:
        void run() {
            BSONObjExternalSorter sorter( _arbitrarySort, 2000 );
            for ( int i=0; i<10000; i++ ) {
                sorter.add( BSON( "x" << rand() % 10000 ), DiskLoc( 5, i ), false );
            }

            sorter.sort( false );

            auto_ptr<BSONObjExternalSorter::Iterator> i = sorter.iterator();
            int num=0;
            double prev = 0;
            while ( i->more() ) {
                pair<BSONObj,DiskLoc> p = i->next();
                num++;
                double cur = p.first["x"].number();
                ASSERT( cur >= prev );
                prev = cur;
            }
            ASSERT_EQUALS( 10000, num );
        }
    };

    /** Sort 1e5 values. */
    class Sort1e5 {
    public:
        void run() {
            const int total = 100000;
            BSONObjExternalSorter sorter( _arbitrarySort, total * 2 );
            for ( int i=0; i<total; i++ ) {
                sorter.add( BSON( "a" << "b" ), DiskLoc( 5, i ), false );
            }

            sorter.sort( false );

            auto_ptr<BSONObjExternalSorter::Iterator> i = sorter.iterator();
            int num=0;
            double prev = 0;
            while ( i->more() ) {
                pair<BSONObj,DiskLoc> p = i->next();
                num++;
                double cur = p.first["x"].number();
                ASSERT( cur >= prev );
                prev = cur;
            }
            ASSERT_EQUALS( total, num );
            ASSERT( sorter.numFiles() > 2 );
        }
    };

    /** Sort 1e6 values. */
    class Sort1e6 {
    public:
        void run() {
            const int total = 1000 * 1000;
            BSONObjExternalSorter sorter( _arbitrarySort, total * 2 );
            for ( int i=0; i<total; i++ ) {
                sorter.add( BSON( "abcabcabcabd" << "basdasdasdasdasdasdadasdasd" << "x" << i ),
                            DiskLoc( 5, i ),
                            false );
            }

            sorter.sort( false );

            auto_ptr<BSONObjExternalSorter::Iterator> i = sorter.iterator();
            int num=0;
            double prev = 0;
            while ( i->more() ) {
                pair<BSONObj,DiskLoc> p = i->next();
                num++;
                double cur = p.first["x"].number();
                ASSERT( cur >= prev );
                prev = cur;
            }
            ASSERT_EQUALS( total, num );
            ASSERT( sorter.numFiles() > 2 );
        }
    };

    /** Sort null valued keys. */
    class SortNull {
    public:
        void run() {

            BSONObjBuilder b;
            b.appendNull("");
            BSONObj x = b.obj();

            BSONObjExternalSorter sorter( _arbitrarySort );
            sorter.add(x, DiskLoc(3,7), false);
            sorter.add(x, DiskLoc(4,7), false);
            sorter.add(x, DiskLoc(2,7), false);
            sorter.add(x, DiskLoc(1,7), false);
            sorter.add(x, DiskLoc(3,77), false);

            sorter.sort( false );

            auto_ptr<BSONObjExternalSorter::Iterator> i = sorter.iterator();
            while( i->more() ) {
                ExternalSortDatum d = i->next();
            }
        }
    };

    /** Sort 130 keys and check their exact values. */
    class Sort130 {
    public:
        void run() {
            // Create a sorter.
            BSONObjExternalSorter sorter(_aFirstSort);
            // Add keys to the sorter.
            int32_t nDocs = 130;
            for( int32_t i = 0; i < nDocs; ++i ) {
                // Insert values in reverse order, for subsequent sort.
                sorter.add( BSON( "" << ( nDocs - 1 - i ) ), /* dummy disk loc */ DiskLoc(), true );
            }
            // The sorter's footprint is now positive.
            ASSERT( sorter.getCurSizeSoFar() > 0 );
            // Sort the keys.
            sorter.sort( true );
            // Check that the keys have been sorted.
            auto_ptr<BSONObjExternalSorter::Iterator> iterator = sorter.iterator();
            int32_t expectedKey = 0;
            while( iterator->more() ) {
                ASSERT_EQUALS( BSON( "" << expectedKey++ ), iterator->next().first );
            }
            ASSERT_EQUALS( nDocs, expectedKey );
        }
    };

    /**
     * BSONObjExternalSorter::add() aborts if the current operation is interrupted, even if storage
     * system writes have occurred.
     */
    class InterruptAdd {
    public:
        InterruptAdd( bool mayInterrupt ) :
            _mayInterrupt( mayInterrupt ) {
        }
        void run() {
            _client.createCollection( _ns );
            // Take a write lock.
            Client::WriteContext ctx( _ns );
            // Do a write to ensure the implementation will interrupt sort() even after a write has
            // occurred.
            BSONObj newDoc;
            theDataFileMgr.insertWithObjMod( _ns, newDoc );
            // Create a sorter with a max file size of only 10k, to trigger a file flush after a
            // relatively small number of inserts.
            auto_ptr<ExternalSortComparison> cmp(BtreeBasedBuilder::getComparison(0,
                BSON("a" << 1)));
            BSONObjExternalSorter sorter(cmp.get(), 10 * 1024 );
            // Register a request to kill the current operation.
            cc().curop()->kill();
            if (_mayInterrupt) {
                // When enough keys are added to fill the first file, an interruption will be
                // triggered as the records are sorted for the file.
                ASSERT_THROWS( addKeysUntilFileFlushed( &sorter, _mayInterrupt ), UserException );
            }
            else {
                // When enough keys are added to fill the first file, an interruption when the
                // records are sorted for the file is prevented because mayInterrupt == false.
                addKeysUntilFileFlushed( &sorter, _mayInterrupt );
            }
        }
    private:
        static void addKeysUntilFileFlushed( BSONObjExternalSorter* sorter, bool mayInterrupt ) {
            while( sorter->numFiles() == 0 ) {
                sorter->add( BSON( "" << 1 ), /* dummy disk loc */ DiskLoc(), mayInterrupt );
            }
        }
        bool _mayInterrupt;
    };

    /**
     * BSONObjExternalSorter::sort() aborts if the current operation is interrupted, even if storage
     * system writes have occurred.
     */
    class InterruptSort {
    public:
        InterruptSort( bool mayInterrupt ) :
            _mayInterrupt( mayInterrupt ) {
        }
        void run() {
            _client.createCollection( _ns );
            // Take a write lock.
            Client::WriteContext ctx( _ns );
            // Do a write to ensure the implementation will interrupt sort() even after a write has
            // occurred.
            BSONObj newDoc;
            theDataFileMgr.insertWithObjMod( _ns, newDoc );
            // Create a sorter.
            BSONObjExternalSorter sorter(_aFirstSort);
            // Add keys to the sorter.
            int32_t nDocs = 130;
            for( int32_t i = 0; i < nDocs; ++i ) {
                sorter.add( BSON( "" << i ), /* dummy disk loc */ DiskLoc(), false );
            }
            ASSERT( sorter.getCurSizeSoFar() > 0 );
            // Register a request to kill the current operation.
            cc().curop()->kill();
            if (_mayInterrupt) {
                // The sort is aborted due to the kill request.
                ASSERT_THROWS( {
                    sorter.sort( _mayInterrupt );
                    auto_ptr<BSONObjExternalSorter::Iterator> iter = sorter.iterator();
                    while (iter->more()) {
                        iter->next();
                    }
                }, UserException );
            }
            else {
                // Sort the keys.
                sorter.sort( _mayInterrupt );
                // Check that the keys have been sorted.
                auto_ptr<BSONObjExternalSorter::Iterator> iterator = sorter.iterator();
                int32_t expectedKey = 0;
                while( iterator->more() ) {
                    ASSERT_EQUALS( BSON( "" << expectedKey++ ), iterator->next().first );
                }
                ASSERT_EQUALS( nDocs, expectedKey );
            }
        }
    private:
        bool _mayInterrupt;
    };

    //
    // Sorter framework testing utilities
    //

    class IntWrapper {
    public:
        IntWrapper(int i=0) :_i(i) {}
        operator const int& () const { return _i; }

        /// members for Sorter
        struct SorterDeserializeSettings {}; // unused
        void serializeForSorter(BufBuilder& buf) const { buf.appendNum(_i); }
        static IntWrapper deserializeForSorter(BufReader& buf, const SorterDeserializeSettings&) {
            return buf.read<int>();
        }
        int memUsageForSorter() const { return sizeof(IntWrapper); }
        IntWrapper getOwned() const { return *this; }
    private:
        int _i;
    };

    typedef pair<IntWrapper, IntWrapper> IWPair;
    typedef SortIteratorInterface<IntWrapper, IntWrapper> IWIterator;
    typedef Sorter<IntWrapper, IntWrapper> IWSorter;

    enum Direction {ASC=1, DESC=-1};
    class IWComparator {
    public:
        IWComparator(Direction dir=ASC) :_dir(dir) {}
        int operator() (const IWPair& lhs, const IWPair& rhs) const {
            if (lhs.first == rhs.first) return 0;
            if (lhs.first <  rhs.first) return -1 * _dir;
            return 1 * _dir;
        }
    private:
        Direction _dir;
    };

    class IntIterator : public IWIterator {
    public:
        IntIterator(int start=0, int stop=INT_MAX, int increment=1)
            : _current(start)
            , _increment(increment)
            , _stop(stop)
        {}
        bool more() {
            if (_increment == 0) return true;
            if (_increment > 0) return _current < _stop;
            return _current > _stop;
        }
        IWPair next() {
            IWPair out(_current, -_current);
            _current += _increment;
            return out;
        }

    private:
        int _current;
        int _increment;
        int _stop;
    };

    class EmptyIterator : public IWIterator {
    public:
        bool more() { return false; }
        Data next() { verify(false); }
    };

    class LimitIterator : public IWIterator {
    public:
        LimitIterator(long long limit, boost::shared_ptr<IWIterator> source)
            : _remaining(limit)
            , _source(source)
        { verify(limit > 0); }

        bool more() { return _remaining && _source->more(); }
        Data next() {
            verify(more());
            _remaining--;
            return _source->next();
        }

    private:
        long long _remaining;
        boost::shared_ptr<IWIterator> _source;
    };

    template <typename It1, typename It2>
    void _assertIteratorsEquivalent(It1 it1, It2 it2, int line) {
        int iteration;
        try {
            for (iteration = 0; true; iteration++) {
                ASSERT_EQUALS(it1->more(), it2->more());
                ASSERT_EQUALS(it1->more(), it2->more()); // make sure more() is safe to call twice
                if (!it1->more())
                    return;

                IWPair pair1 = it1->next();
                IWPair pair2 = it2->next();
                ASSERT_EQUALS(pair1.first, pair2.first);
                ASSERT_EQUALS(pair1.second, pair2.second);
            }

        } catch (...) {
            mongo::unittest::log() <<
                "Failure from line " << line << " on iteration " << iteration << endl;
            throw;
        }
    }
#define ASSERT_ITERATORS_EQUIVALENT(it1, it2) _assertIteratorsEquivalent(it1, it2, __LINE__)

    template <int N>
    boost::shared_ptr<IWIterator> makeInMemIterator(const int (&array)[N]) {
        vector<IWPair> vec;
        for (int i=0; i<N; i++)
            vec.push_back(IWPair(array[i], -array[i]));
        return boost::make_shared<sorter::InMemIterator<IntWrapper, IntWrapper> >(vec);
    }

    template <typename IteratorPtr, int N>
    boost::shared_ptr<IWIterator> mergeIterators(IteratorPtr (&array)[N],
                                                 Direction Dir=ASC,
                                                 const SortOptions& opts=SortOptions()) {
        vector<boost::shared_ptr<IWIterator> > vec;
        for (int i=0; i<N; i++)
            vec.push_back(boost::shared_ptr<IWIterator>(array[i]));
        return boost::shared_ptr<IWIterator>(IWIterator::merge(vec, opts, IWComparator(Dir)));
    }

    //
    // Tests for Sorter framework internals
    //

    class InMemIterTests {
        public:
        void run() {
            {
                EmptyIterator empty;
                sorter::InMemIterator<IntWrapper, IntWrapper> inMem;
                ASSERT_ITERATORS_EQUIVALENT(&inMem, &empty);
            }
            {
                static const int zeroUpTo20[] = {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19};
                ASSERT_ITERATORS_EQUIVALENT(makeInMemIterator(zeroUpTo20),
                                            make_shared<IntIterator>(0,20));
            }
            {
                // make sure InMemIterator doesn't do any reordering on it's own
                static const int unsorted[] = {6,3,7,4,0,9,5,7,1,8};
                class UnsortedIter : public IWIterator {
                public:
                    UnsortedIter() :_pos(0) {}
                    bool more() { return _pos < sizeof(unsorted)/sizeof(unsorted[0]); }
                    IWPair next() {
                        IWPair ret(unsorted[_pos], -unsorted[_pos]);
                        _pos++;
                        return ret;
                    }
                    size_t _pos;
                } unsortedIter;

                ASSERT_ITERATORS_EQUIVALENT(makeInMemIterator(unsorted),
                                            static_cast<IWIterator*>(&unsortedIter));
            }
        }
    };

    class SortedFileWriterAndFileIteratorTests {
    public:
        void run() {
            { // small
                SortedFileWriter<IntWrapper, IntWrapper> sorter;
                sorter.addAlreadySorted(0,0);
                sorter.addAlreadySorted(1,-1);
                sorter.addAlreadySorted(2,-2);
                sorter.addAlreadySorted(3,-3);
                sorter.addAlreadySorted(4,-4);
                ASSERT_ITERATORS_EQUIVALENT(boost::shared_ptr<IWIterator>(sorter.done()),
                                            make_shared<IntIterator>(0,5));
            }
            { // big
                SortedFileWriter<IntWrapper, IntWrapper> sorter;
                for (int i=0; i< 10*1000*1000; i++)
                    sorter.addAlreadySorted(i,-i);

                ASSERT_ITERATORS_EQUIVALENT(boost::shared_ptr<IWIterator>(sorter.done()),
                                            make_shared<IntIterator>(0,10*1000*1000));
            }
        }
    };



    class MergeIteratorTests {
    public:
        void run() {
            { // test empty (no inputs)
                vector<boost::shared_ptr<IWIterator> > vec;
                boost::shared_ptr<IWIterator> mergeIter (IWIterator::merge(vec,
                                                                           SortOptions(),
                                                                           IWComparator()));
                ASSERT_ITERATORS_EQUIVALENT(mergeIter,
                                            make_shared<EmptyIterator>());
            }
            { // test empty (only empty inputs)
                boost::shared_ptr<IWIterator> iterators[] =
                    { make_shared<EmptyIterator>()
                    , make_shared<EmptyIterator>()
                    , make_shared<EmptyIterator>()
                    };

                ASSERT_ITERATORS_EQUIVALENT(mergeIterators(iterators, ASC),
                                            make_shared<EmptyIterator>());
            }

            { // test ASC
                boost::shared_ptr<IWIterator> iterators[] =
                    { make_shared<IntIterator>(1, 20, 2) // 1, 3, ... 19
                    , make_shared<IntIterator>(0, 20, 2) // 0, 2, ... 18
                    };

                ASSERT_ITERATORS_EQUIVALENT(mergeIterators(iterators, ASC),
                                            make_shared<IntIterator>(0,20,1));
            }

            { // test DESC with an empty source
                boost::shared_ptr<IWIterator> iterators[] =
                    { make_shared<IntIterator>(30, 0, -3) // 30, 27, ... 3
                    , make_shared<IntIterator>(29, 0, -3) // 29, 26, ... 2
                    , make_shared<IntIterator>(28, 0, -3) // 28, 25, ... 1
                    , make_shared<EmptyIterator>()
                    };

                ASSERT_ITERATORS_EQUIVALENT(mergeIterators(iterators, DESC),
                                            make_shared<IntIterator>(30,0,-1));
            }
            { // test Limit
                boost::shared_ptr<IWIterator> iterators[] =
                    { make_shared<IntIterator>(1, 20, 2) // 1, 3, ... 19
                    , make_shared<IntIterator>(0, 20, 2) // 0, 2, ... 18
                    };

                ASSERT_ITERATORS_EQUIVALENT(
                        mergeIterators(iterators, ASC, SortOptions().Limit(10)),
                        make_shared<LimitIterator>(10, make_shared<IntIterator>(0,20,1)));
            }
        }
    };

    namespace SorterTests {
        class Basic {
        public:
            virtual ~Basic() {}

            void run() {
                { // test empty (no limit)
                    ASSERT_ITERATORS_EQUIVALENT(done(makeSorter()),
                                                make_shared<EmptyIterator>());
                }
                { // test empty (limit 1)
                    ASSERT_ITERATORS_EQUIVALENT(done(makeSorter(SortOptions().Limit(1))),
                                                make_shared<EmptyIterator>());
                }
                { // test empty (limit 10)
                    ASSERT_ITERATORS_EQUIVALENT(done(makeSorter(SortOptions().Limit(10))),
                                                make_shared<EmptyIterator>());
                }

                { // test all data ASC
                    boost::shared_ptr<IWSorter> sorter = makeSorter(SortOptions(),
                                                                    IWComparator(ASC));
                    addData(sorter);
                    ASSERT_ITERATORS_EQUIVALENT(done(sorter), correct());
                }
                { // test all data DESC
                    boost::shared_ptr<IWSorter> sorter = makeSorter(SortOptions(),
                                                                    IWComparator(DESC));
                    addData(sorter);
                    ASSERT_ITERATORS_EQUIVALENT(done(sorter), correctReverse());
                }

                // The MSVC++ STL includes extra checks in debug mode that make these tests too
                // slow to run. Among other things, they make all heap functions O(N) not O(logN).
#if !(defined(_MSC_VER) && defined(_DEBUG))
                { // merge all data ASC
                    boost::shared_ptr<IWSorter> sorters[] = {
                        makeSorter(SortOptions(), IWComparator(ASC)),
                        makeSorter(SortOptions(), IWComparator(ASC))
                    };

                    addData(sorters[0]);
                    addData(sorters[1]);

                    boost::shared_ptr<IWIterator> iters1[] = {done(sorters[0]), done(sorters[1])};
                    boost::shared_ptr<IWIterator> iters2[] = {correct(), correct()};
                    ASSERT_ITERATORS_EQUIVALENT(mergeIterators(iters1, ASC),
                                                mergeIterators(iters2, ASC));
                }
                { // merge all data DESC and use multiple threads to insert
                    boost::shared_ptr<IWSorter> sorters[] = {
                        makeSorter(SortOptions(), IWComparator(DESC)),
                        makeSorter(SortOptions(), IWComparator(DESC))
                    };

                    boost::thread inBackground(&Basic::addData, this, sorters[0]);
                    addData(sorters[1]);
                    inBackground.join();

                    boost::shared_ptr<IWIterator> iters1[] = {done(sorters[0]), done(sorters[1])};
                    boost::shared_ptr<IWIterator> iters2[] = {correctReverse(), correctReverse()};
                    ASSERT_ITERATORS_EQUIVALENT(mergeIterators(iters1, DESC),
                                                mergeIterators(iters2, DESC));
                }
#endif
            }

            // add data to the sorter
            virtual void addData(ptr<IWSorter> sorter) {
                sorter->add(2,-2);
                sorter->add(1,-1);
                sorter->add(0,0);
                sorter->add(4,-4);
                sorter->add(3,-3);
            }

            // returns an iterator with the correct results
            virtual boost::shared_ptr<IWIterator> correct() {
                return make_shared<IntIterator>(0,5); // 0, 1, ... 4
            }

            // like correct but with opposite sort direction
            virtual boost::shared_ptr<IWIterator> correctReverse() {
                return make_shared<IntIterator>(4,-1,-1); // 4, 3, ... 0
            }

            // It is safe to ignore / overwrite any part of options
            virtual SortOptions adjustSortOptions(SortOptions opts) {
                return opts;
            }

        private:

            // Make a new sorter with desired opts and comp. Opts may be ignored but not comp
            boost::shared_ptr<IWSorter> makeSorter(SortOptions opts=SortOptions(),
                                                   IWComparator comp=IWComparator(ASC)) {
                return boost::shared_ptr<IWSorter>(IWSorter::make(adjustSortOptions(opts), comp));
            }

            boost::shared_ptr<IWIterator> done(ptr<IWSorter> sorter) {
                return boost::shared_ptr<IWIterator>(sorter->done());
            }
        };

        class Limit : public Basic {
            virtual SortOptions adjustSortOptions(SortOptions opts) {
                return opts.Limit(5);
            }
            void addData(ptr<IWSorter> sorter) {
                sorter->add(0,0);
                sorter->add(3,-3);
                sorter->add(4,-4);
                sorter->add(2,-2);
                sorter->add(1,-1);
                sorter->add(-1,1);
            }
            virtual boost::shared_ptr<IWIterator> correct() {
                return make_shared<IntIterator>(-1,4);
            }
            virtual boost::shared_ptr<IWIterator> correctReverse() {
                return make_shared<IntIterator>(4,-1,-1);
            }
        };

        class Dupes : public Basic {
            void addData(ptr<IWSorter> sorter) {
                sorter->add(1,-1);
                sorter->add(-1,1);
                sorter->add(1,-1);
                sorter->add(-1,1);
                sorter->add(1,-1);
                sorter->add(0,0);
                sorter->add(2,-2);
                sorter->add(-1,1);
                sorter->add(2,-2);
                sorter->add(3,-3);
            }
            virtual boost::shared_ptr<IWIterator> correct() {
                const int array[] = {-1,-1,-1, 0, 1,1,1, 2,2, 3};
                return makeInMemIterator(array);
            }
            virtual boost::shared_ptr<IWIterator> correctReverse() {
                const int array[] = {3, 2,2, 1,1,1, 0, -1,-1,-1};
                return makeInMemIterator(array);
            }
        };

        template <bool Random=true>
        class LotsOfDataLittleMemory : public Basic {
        public:
            LotsOfDataLittleMemory() :_array(new int[NUM_ITEMS]) {
                for (int i=0; i<NUM_ITEMS; i++)
                    _array[i] = i;

                if (Random)
                    std::random_shuffle(_array.get(), _array.get()+NUM_ITEMS);
            }

            SortOptions adjustSortOptions(SortOptions opts) {
                // Make sure we use a reasonable number of files when we spill
                BOOST_STATIC_ASSERT((NUM_ITEMS * sizeof(IWPair)) / MEM_LIMIT > 50);
                BOOST_STATIC_ASSERT((NUM_ITEMS * sizeof(IWPair)) / MEM_LIMIT < 500);

                return opts.MaxMemoryUsageBytes(MEM_LIMIT).ExtSortAllowed();
            }

            void addData(ptr<IWSorter> sorter) {
                for (int i=0; i<NUM_ITEMS; i++)
                    sorter->add(_array[i], -_array[i]);

                if (typeid(*this) == typeid(LotsOfDataLittleMemory)) {
                    // don't do this check in subclasses since they may set a limit
                    ASSERT_GREATER_THAN_OR_EQUALS(static_cast<size_t>(sorter->numFiles()),
                                                  (NUM_ITEMS * sizeof(IWPair)) / MEM_LIMIT);
                }
            }

            virtual boost::shared_ptr<IWIterator> correct() {
                return make_shared<IntIterator>(0, NUM_ITEMS);
            }
            virtual boost::shared_ptr<IWIterator> correctReverse() {
                return make_shared<IntIterator>(NUM_ITEMS-1, -1, -1);
            }

            enum Constants {
                NUM_ITEMS = 10*1000*1000,
                MEM_LIMIT = 1024*1024,
            };
            boost::scoped_array<int> _array;
        };


        template <long long Limit, bool Random=true>
        class LotsOfDataWithLimit : public LotsOfDataLittleMemory<Random> {
            typedef LotsOfDataLittleMemory<Random> Parent;
            SortOptions adjustSortOptions(SortOptions opts) {
                // Make sure our tests will spill or not as desired
                BOOST_STATIC_ASSERT(MEM_LIMIT / 2 > (     100 * sizeof(IWPair)));
                BOOST_STATIC_ASSERT(MEM_LIMIT     < (100*1000 * sizeof(IWPair)));

                // Make sure we use a reasonable number of files when we spill
                BOOST_STATIC_ASSERT((Parent::NUM_ITEMS * sizeof(IWPair)) / MEM_LIMIT > 100);
                BOOST_STATIC_ASSERT((Parent::NUM_ITEMS * sizeof(IWPair)) / MEM_LIMIT < 500);

                return opts.MaxMemoryUsageBytes(MEM_LIMIT).ExtSortAllowed().Limit(Limit);
            }
            virtual boost::shared_ptr<IWIterator> correct() {
                return make_shared<LimitIterator>(Limit, Parent::correct());
            }
            virtual boost::shared_ptr<IWIterator> correctReverse() {
                return make_shared<LimitIterator>(Limit, Parent::correctReverse());
            }
            enum { MEM_LIMIT = 512*1024 };
        };
    }



    class ExtSortTests : public Suite {
    public:
        ExtSortTests() :
            Suite( "extsort" ) {
        }

        void setupTests() {
            add<SortFour>();
            add<SortFourCheckDiskLoc>();
            add<SortNone>();
            add<SortByDiskLock>();
            add<Sort1e4>();
            add<Sort1e5>();
            add<Sort1e6>();
            add<SortNull>();
            add<Sort130>();
            add<InterruptAdd>( false );
            add<InterruptAdd>( true );
            add<InterruptSort>( false );
            add<InterruptSort>( true );

            add<InMemIterTests>();
            add<SortedFileWriterAndFileIteratorTests>();
            add<MergeIteratorTests>();
            add<SorterTests::Basic>();
            add<SorterTests::Limit>();
            add<SorterTests::Dupes>();
            add<SorterTests::LotsOfDataLittleMemory</*random=*/false> >();
            add<SorterTests::LotsOfDataLittleMemory</*random=*/true> >();
            add<SorterTests::LotsOfDataWithLimit<1,/*random=*/false> >(); // limit=1 is special case
            add<SorterTests::LotsOfDataWithLimit<1,/*random=*/true> >();  // limit=1 is special case
            add<SorterTests::LotsOfDataWithLimit<100,/*random=*/false> >(); // fits in mem
            add<SorterTests::LotsOfDataWithLimit<100,/*random=*/true> >();  // fits in mem
            add<SorterTests::LotsOfDataWithLimit<100*1000,/*random=*/false> >(); // spills
            add<SorterTests::LotsOfDataWithLimit<100*1000,/*random=*/true> >(); // spills
        }
    } extSortTests;

} // namespace ExtSortTests
