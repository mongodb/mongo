    typedef BtreeBucket::_KeyNode _KeyNode;
 
    const char* ns() {
        return "unittests.btreetests";
    }

    // dummy, valid record loc
    const DiskLoc recordLoc() {
        return DiskLoc( 0, 2 );
    }

    class Ensure {
    public:
        Ensure() {
            _c.ensureIndex( ns(), BSON( "a" << 1 ), false, "testIndex",
                false, // given two versions not sure if cache true would mess us up...
                false, BTVERSION);
        }
        ~Ensure() {
            _c.dropCollection( ns() );
            //_c.dropIndexes( ns() );
        }
    private:
        DBDirectClient _c;
    };

    class Base : public Ensure {
    public:
        Base() :
            _context( ns() ) {
            {
                bool f = false;
                verify( f = true );
                massert( 10402 , "verify is misdefined", f);
            }
        }
        virtual ~Base() {}
        static string bigNumString( long long n, int len = 800 ) {
            char sub[17];
            sprintf( sub, "%.16llx", n );
            string val( len, ' ' );
            for( int i = 0; i < len; ++i ) {
                val[ i ] = sub[ i % 16 ];
            }
            return val;
        }
    protected:
        const BtreeBucket* bt() {
            return id().head.btree();
        }
        DiskLoc dl() {
            return id().head;
        }
        IndexDetails& id() {
            NamespaceDetails *nsd = nsdetails( ns() );
            verify( nsd );
            return nsd->idx( 1 );
        }
        void checkValid( int nKeys ) {
            ASSERT( bt() );
            ASSERT( bt()->isHead() );
            bt()->assertValid( order(), true );
            ASSERT_EQUALS( nKeys, bt()->fullValidate( dl(), order(), 0, true ) );
        }
        void dump() {
            bt()->dumpTree( dl(), order() );
        }
        void insert( BSONObj &key ) {
            const BtreeBucket *b = bt();

#if defined(TESTTWOSTEP)
            {
                Continuation c(dl(), recordLoc(), key, Ordering::make(order()), id());
                b->twoStepInsert(dl(), c, true);
                c.doIndexInsertionWrites();
            }
#else
            {
                b->bt_insert( dl(), recordLoc(), key, Ordering::make(order()), true, id(), true );
            }
#endif
            getDur().commitIfNeeded();
        }
        bool unindex( BSONObj &key ) {
            getDur().commitIfNeeded();
            return bt()->unindex( dl(), id(), key, recordLoc() );
        }
        static BSONObj simpleKey( char c, int n = 1 ) {
            BSONObjBuilder builder;
            string val( n, c );
            builder.append( "a", val );
            return builder.obj();
        }
        void locate( BSONObj &key, int expectedPos,
                     bool expectedFound, const DiskLoc &expectedLocation,
                     int direction = 1 ) {
            int pos;
            bool found;
            DiskLoc location =
                bt()->locate( id(), dl(), key, Ordering::make(order()), pos, found, recordLoc(), direction );
            ASSERT_EQUALS( expectedFound, found );
            ASSERT( location == expectedLocation );
            ASSERT_EQUALS( expectedPos, pos );
        }
        bool present( BSONObj &key, int direction ) {
            int pos;
            bool found;
            bt()->locate( id(), dl(), key, Ordering::make(order()), pos, found, recordLoc(), direction );
            return found;
        }
        BSONObj order() {
            return id().keyPattern();
        }
        const BtreeBucket *child( const BtreeBucket *b, int i ) {
            verify( i <= b->nKeys() );
            DiskLoc d;
            if ( i == b->nKeys() ) {
                d = b->getNextChild();
            }
            else {
                d = b->keyNode( i ).prevChildBucket;
            }
            verify( !d.isNull() );
            return d.btree();
        }
        void checkKey( char i ) {
            stringstream ss;
            ss << i;
            checkKey( ss.str() );
        }
        void checkKey( const string &k ) {
            BSONObj key = BSON( "" << k );
//            log() << "key: " << key << endl;
            ASSERT( present( key, 1 ) );
            ASSERT( present( key, -1 ) );
        }
    private:
        Lock::GlobalWrite lk_;
        Client::Context _context;
    };

    class Create : public Base {
    public:
        Create() { }
        void run() {
            checkValid( 0 );
        }
    };

    class SimpleInsertDelete : public Base {
    public:
        void run() {
            BSONObj key = simpleKey( 'z' );
            insert( key );

            checkValid( 1 );
            locate( key, 0, true, dl() );

            unindex( key );

            checkValid( 0 );
            locate( key, 0, false, DiskLoc() );
        }
    };

    class SplitUnevenBucketBase : public Base {
    public:
        virtual ~SplitUnevenBucketBase() {}
        void run() {
            for ( int i = 0; i < 10; ++i ) {
                BSONObj shortKey = simpleKey( shortToken( i ), 1 );
                insert( shortKey );
                BSONObj longKey = simpleKey( longToken( i ), 800 );
                insert( longKey );
            }
            checkValid( 20 );
            ASSERT_EQUALS( 1, bt()->nKeys() );
            checkSplit();
        }
    protected:
        virtual char shortToken( int i ) const = 0;
        virtual char longToken( int i ) const = 0;
        static char leftToken( int i ) {
            return 'a' + i;
        }
        static char rightToken( int i ) {
            return 'z' - i;
        }
        virtual void checkSplit() = 0;
    };

    class SplitRightHeavyBucket : public SplitUnevenBucketBase {
    private:
        virtual char shortToken( int i ) const {
            return leftToken( i );
        }
        virtual char longToken( int i ) const {
            return rightToken( i );
        }
        virtual void checkSplit() {
            ASSERT_EQUALS( 15, child( bt(), 0 )->nKeys() );
            ASSERT_EQUALS( 4, child( bt(), 1 )->nKeys() );
        }
    };

    class SplitLeftHeavyBucket : public SplitUnevenBucketBase {
    private:
        virtual char shortToken( int i ) const {
            return rightToken( i );
        }
        virtual char longToken( int i ) const {
            return leftToken( i );
        }
        virtual void checkSplit() {
            ASSERT_EQUALS( 4, child( bt(), 0 )->nKeys() );
            ASSERT_EQUALS( 15, child( bt(), 1 )->nKeys() );
        }
    };

    class MissingLocate : public Base {
    public:
        void run() {
            for ( int i = 0; i < 3; ++i ) {
                BSONObj k = simpleKey( 'b' + 2 * i );
                insert( k );
            }

            locate( 1, 'a', 'b', dl() );
            locate( 1, 'c', 'd', dl() );
            locate( 1, 'e', 'f', dl() );
            locate( 1, 'g', 'g' + 1, DiskLoc() ); // of course, 'h' isn't in the index.

            // old behavior
            //       locate( -1, 'a', 'b', dl() );
            //       locate( -1, 'c', 'd', dl() );
            //       locate( -1, 'e', 'f', dl() );
            //       locate( -1, 'g', 'f', dl() );

            locate( -1, 'a', 'a' - 1, DiskLoc() ); // of course, 'a' - 1 isn't in the index
            locate( -1, 'c', 'b', dl() );
            locate( -1, 'e', 'd', dl() );
            locate( -1, 'g', 'f', dl() );
        }
    private:
        void locate( int direction, char token, char expectedMatch,
                     DiskLoc expectedLocation ) {
            BSONObj k = simpleKey( token );
            int expectedPos = ( expectedMatch - 'b' ) / 2;
            Base::locate( k, expectedPos, false, expectedLocation, direction );
        }
    };

    class MissingLocateMultiBucket : public Base {
    public:
        void run() {
            for ( int i = 0; i < 8; ++i ) {
                insert( i );
            }
            insert( 9 );
            insert( 8 );
//            dump();
            BSONObj straddle = key( 'i' );
            locate( straddle, 0, false, dl(), 1 );
            straddle = key( 'k' );
            locate( straddle, 0, false, dl(), -1 );
        }
    private:
        BSONObj key( char c ) {
            return simpleKey( c, 800 );
        }
        void insert( int i ) {
            BSONObj k = key( 'b' + 2 * i );
            Base::insert( k );
        }
    };

    class SERVER983 : public Base {
    public:
        void run() {
            for ( int i = 0; i < 10; ++i ) {
                insert( i );
            }
//            dump();
            BSONObj straddle = key( 'o' );
            locate( straddle, 0, false, dl(), 1 );
            straddle = key( 'q' );
            locate( straddle, 0, false, dl(), -1 );
        }
    private:
        BSONObj key( char c ) {
            return simpleKey( c, 800 );
        }
        void insert( int i ) {
            BSONObj k = key( 'b' + 2 * i );
            Base::insert( k );
        }
    };

    class DontReuseUnused : public Base {
    public:
        void run() {
            for ( int i = 0; i < 10; ++i ) {
                insert( i );
            }
//            dump();
            BSONObj root = key( 'p' );
            unindex( root );
            Base::insert( root );
            locate( root, 0, true, bt()->getNextChild(), 1 );
        }
    private:
        BSONObj key( char c ) {
            return simpleKey( c, 800 );
        }
        void insert( int i ) {
            BSONObj k = key( 'b' + 2 * i );
            Base::insert( k );
        }
    };

    class PackUnused : public Base {
    public:
        void run() {
            for ( long long i = 0; i < 1000000; i += 1000 ) {
                insert( i );
            }
            string orig, after;
            {
                stringstream ss;
                bt()->shape( ss );
                orig = ss.str();
            }
            vector< string > toDel;
            vector< string > other;
            BSONObjBuilder start;
            start.appendMinKey( "a" );
            BSONObjBuilder end;
            end.appendMaxKey( "a" );
            auto_ptr< BtreeCursor > c( BtreeCursor::make( nsdetails( ns() ), 1, id(), start.done(), end.done(), false, 1 ) );
            while( c->ok() ) {
                if ( c->curKeyHasChild() ) {
                    toDel.push_back( c->currKey().firstElement().valuestr() );
                }
                else {
                    other.push_back( c->currKey().firstElement().valuestr() );
                }
                c->advance();
            }
            ASSERT( toDel.size() > 0 );
            for( vector< string >::const_iterator i = toDel.begin(); i != toDel.end(); ++i ) {
                BSONObj o = BSON( "a" << *i );
                unindex( o );
            }
            ASSERT( other.size() > 0 );
            for( vector< string >::const_iterator i = other.begin(); i != other.end(); ++i ) {
                BSONObj o = BSON( "a" << *i );
                unindex( o );
            }

            long long unused = 0;
            ASSERT_EQUALS( 0, bt()->fullValidate( dl(), order(), &unused, true ) );

            for ( long long i = 50000; i < 50100; ++i ) {
                insert( i );
            }

            long long unused2 = 0;
            ASSERT_EQUALS( 100, bt()->fullValidate( dl(), order(), &unused2, true ) );

//            log() << "old unused: " << unused << ", new unused: " << unused2 << endl;
//
            ASSERT( unused2 <= unused );
        }
    protected:
        void insert( long long n ) {
            string val = bigNumString( n );
            BSONObj k = BSON( "a" << val );
            Base::insert( k );
        }
    };

    class DontDropReferenceKey : public PackUnused {
    public:
        void run() {
            // with 80 root node is full
            for ( long long i = 0; i < 80; i += 1 ) {
                insert( i );
            }

            BSONObjBuilder start;
            start.appendMinKey( "a" );
            BSONObjBuilder end;
            end.appendMaxKey( "a" );
            BSONObj l = bt()->keyNode( 0 ).key.toBson();
            string toInsert;
            auto_ptr< BtreeCursor > c( BtreeCursor::make( nsdetails( ns() ), 1, id(), start.done(), end.done(), false, 1 ) );
            while( c->ok() ) {
                if ( c->currKey().woCompare( l ) > 0 ) {
                    toInsert = c->currKey().firstElement().valuestr();
                    break;
                }
                c->advance();
            }
            // too much work to try to make this happen through inserts and deletes
            // we are intentionally manipulating the btree bucket directly here
            BtreeBucket::Loc* L = const_cast< BtreeBucket::Loc* >( &bt()->keyNode( 1 ).prevChildBucket );
            getDur().writing(L)->Null();
            getDur().writingInt( const_cast< BtreeBucket::Loc& >( bt()->keyNode( 1 ).recordLoc ).GETOFS() ) |= 1; // make unused
            BSONObj k = BSON( "a" << toInsert );
            Base::insert( k );
        }
    };

    class MergeBuckets : public Base {
    public:
        virtual ~MergeBuckets() {}
        void run() {
            for ( int i = 0; i < 10; ++i ) {
                insert( i );
            }
//            dump();
            string ns = id().indexNamespace();
            ASSERT_EQUALS( 3, nsdetails( ns.c_str() )->stats.nrecords );
            int expectedCount = 10 - unindexKeys();
//            dump();
            ASSERT_EQUALS( 1, nsdetails( ns.c_str() )->stats.nrecords );
            long long unused = 0;
            ASSERT_EQUALS( expectedCount, bt()->fullValidate( dl(), order(), &unused, true ) );
            ASSERT_EQUALS( 0, unused );
        }
    protected:
        BSONObj key( char c ) {
            return simpleKey( c, 800 );
        }
        void insert( int i ) {
            BSONObj k = key( 'b' + 2 * i );
            Base::insert( k );
        }
        virtual int unindexKeys() = 0;
    };

    class MergeBucketsLeft : public MergeBuckets {
        virtual int unindexKeys() {
            BSONObj k = key( 'b' );
            unindex( k );
            k = key( 'b' + 2 );
            unindex( k );
            k = key( 'b' + 4 );
            unindex( k );
            k = key( 'b' + 6 );
            unindex( k );
            return 4;
        }
    };

    class MergeBucketsRight : public MergeBuckets {
        virtual int unindexKeys() {
            BSONObj k = key( 'b' + 2 * 9 );
            unindex( k );
            return 1;
        }
    };

    // deleting from head won't coalesce yet
//    class MergeBucketsHead : public MergeBuckets {
//        virtual BSONObj unindexKey() { return key( 'p' ); }
//    };

    class MergeBucketsDontReplaceHead : public Base {
    public:
        void run() {
            for ( int i = 0; i < 18; ++i ) {
                insert( i );
            }
            //            dump();
            string ns = id().indexNamespace();
            ASSERT_EQUALS( 4, nsdetails( ns.c_str() )->stats.nrecords );
            BSONObj k = key( 'a' + 17 );
            unindex( k );
            ASSERT_EQUALS( 3, nsdetails( ns.c_str() )->stats.nrecords );
            long long unused = 0;
            ASSERT_EQUALS( 17, bt()->fullValidate( dl(), order(), &unused, true ) );
            ASSERT_EQUALS( 0, unused );
        }
    private:
        BSONObj key( char c ) {
            return simpleKey( c, 800 );
        }
        void insert( int i ) {
            BSONObj k = key( 'a' + i );
            Base::insert( k );
        }
    };

    // Tool to construct custom trees for tests.
    class ArtificialTree : public BtreeBucket {
    public:
        void push( const BSONObj &key, const DiskLoc &child ) {
            KeyOwned k(key);
            pushBack( dummyDiskLoc(), k, Ordering::make( BSON( "a" << 1 ) ), child );
        }
        void setNext( const DiskLoc &child ) {
            nextChild = child;
        }
        static DiskLoc make( IndexDetails &id ) {
            DiskLoc ret = addBucket( id );
            is( ret )->init();
            getDur().commitIfNeeded();
            return ret;
        }
        static ArtificialTree *is( const DiskLoc &l ) {
            return static_cast< ArtificialTree * >( l.btreemod() );
        }
        static DiskLoc makeTree( const string &spec, IndexDetails &id ) {
            return makeTree( fromjson( spec ), id );
        }
        static DiskLoc makeTree( const BSONObj &spec, IndexDetails &id ) {
            DiskLoc node = make( id );
            ArtificialTree *n = ArtificialTree::is( node );
            BSONObjIterator i( spec );
            while( i.more() ) {
                BSONElement e = i.next();
                DiskLoc child;
                if ( e.type() == Object ) {
                    child = makeTree( e.embeddedObject(), id );
                }
                if ( e.fieldName() == string( "_" ) ) {
                    n->setNext( child );
                }
                else {
                    n->push( BSON( "" << expectedKey( e.fieldName() ) ), child );
                }
            }
            n->fixParentPtrs( node );
            return node;
        }
        static void setTree( const string &spec, IndexDetails &id ) {
            set( makeTree( spec, id ), id );
        }
        static void set( const DiskLoc &l, IndexDetails &id ) {
            ArtificialTree::is( id.head )->deallocBucket( id.head, id );
            getDur().writingDiskLoc(id.head) = l;
        }
        static string expectedKey( const char *spec ) {
            if ( spec[ 0 ] != '$' ) {
                return spec;
            }
            char *endPtr;
            // parsing a long long is a pain, so just allow shorter keys for now
            unsigned long long num = strtol( spec + 1, &endPtr, 16 );
            int len = 800;
            if( *endPtr == '$' ) {
                len = strtol( endPtr + 1, 0, 16 );
            }
            return Base::bigNumString( num, len );
        }
        static void checkStructure( const BSONObj &spec, const IndexDetails &id, const DiskLoc node ) {
            ArtificialTree *n = ArtificialTree::is( node );
            BSONObjIterator j( spec );
            for( int i = 0; i < n->n; ++i ) {
                ASSERT( j.more() );
                BSONElement e = j.next();
                KeyNode kn = n->keyNode( i );
                string expected = expectedKey( e.fieldName() );
                ASSERT( present( id, BSON( "" << expected ), 1 ) );
                ASSERT( present( id, BSON( "" << expected ), -1 ) );
                ASSERT_EQUALS( expected, kn.key.toBson().firstElement().valuestr() );
                if ( kn.prevChildBucket.isNull() ) {
                    ASSERT( e.type() == jstNULL );
                }
                else {
                    ASSERT( e.type() == Object );
                    checkStructure( e.embeddedObject(), id, kn.prevChildBucket );
                }
            }
            if ( n->nextChild.isNull() ) {
                // maybe should allow '_' field with null value?
                ASSERT( !j.more() );
            }
            else {
                BSONElement e = j.next();
                ASSERT_EQUALS( string( "_" ), e.fieldName() );
                ASSERT( e.type() == Object );
                checkStructure( e.embeddedObject(), id, n->nextChild );
            }
            ASSERT( !j.more() );
        }
        static void checkStructure( const string &spec, const IndexDetails &id ) {
            checkStructure( fromjson( spec ), id, id.head );
        }
        static bool present( const IndexDetails &id, const BSONObj &key, int direction ) {
            int pos;
            bool found;
            id.head.btree()->locate( id, id.head, key, Ordering::make(id.keyPattern()), pos, found, recordLoc(), direction );
            return found;
        }
        int headerSize() const { return BtreeBucket::headerSize(); }
        int packedDataSize( int pos ) const { return BtreeBucket::packedDataSize( pos ); }
        void fixParentPtrs( const DiskLoc &thisLoc ) { BtreeBucket::fixParentPtrs( thisLoc ); }
        void forcePack() {
            topSize += emptySize;
            emptySize = 0;
            setNotPacked();
        }
    private:
        DiskLoc dummyDiskLoc() const { return DiskLoc( 0, 2 ); }
    };

    /**
     * We could probably refactor the following tests, but it's easier to debug
     * them in the present state.
     */

    class MergeBucketsDelInternal : public Base {
    public:
        void run() {
            ArtificialTree::setTree( "{d:{b:{a:null},bb:null,_:{c:null}},_:{f:{e:null},_:{g:null}}}", id() );
//            dump();
            string ns = id().indexNamespace();
            ASSERT_EQUALS( 8, bt()->fullValidate( dl(), order(), 0, true ) );
            ASSERT_EQUALS( 7, nsdetails( ns.c_str() )->stats.nrecords );

            BSONObj k = BSON( "" << "bb" );
            verify( unindex( k ) );
//            dump();
            ASSERT_EQUALS( 7, bt()->fullValidate( dl(), order(), 0, true ) );
            ASSERT_EQUALS( 5, nsdetails( ns.c_str() )->stats.nrecords );
            ArtificialTree::checkStructure( "{b:{a:null},d:{c:null},f:{e:null},_:{g:null}}", id() );
        }
    };

    class MergeBucketsRightNull : public Base {
    public:
        void run() {
            ArtificialTree::setTree( "{d:{b:{a:null},bb:null,cc:{c:null}},_:{f:{e:null},h:{g:null}}}", id() );
//            dump();
            string ns = id().indexNamespace();
            ASSERT_EQUALS( 10, bt()->fullValidate( dl(), order(), 0, true ) );
            ASSERT_EQUALS( 7, nsdetails( ns.c_str() )->stats.nrecords );

            BSONObj k = BSON( "" << "bb" );
            verify( unindex( k ) );
//            dump();
            ASSERT_EQUALS( 9, bt()->fullValidate( dl(), order(), 0, true ) );
            ASSERT_EQUALS( 5, nsdetails( ns.c_str() )->stats.nrecords );
            ArtificialTree::checkStructure( "{b:{a:null},cc:{c:null},d:null,f:{e:null},h:{g:null}}", id() );
        }
    };

    // not yet handling this case
    class DontMergeSingleBucket : public Base {
    public:
        void run() {
            ArtificialTree::setTree( "{d:{b:{a:null},c:null}}", id() );
//            dump();
            string ns = id().indexNamespace();
            ASSERT_EQUALS( 4, bt()->fullValidate( dl(), order(), 0, true ) );
            ASSERT_EQUALS( 3, nsdetails( ns.c_str() )->stats.nrecords );
            BSONObj k = BSON( "" << "c" );
            verify( unindex( k ) );
//            dump();
            ASSERT_EQUALS( 3, bt()->fullValidate( dl(), order(), 0, true ) );
            ASSERT_EQUALS( 3, nsdetails( ns.c_str() )->stats.nrecords );
            ArtificialTree::checkStructure( "{d:{b:{a:null}}}", id() );
        }
    };

    class ParentMergeNonRightToLeft : public Base {
    public:
        void run() {
            ArtificialTree::setTree( "{d:{b:{a:null},bb:null,cc:{c:null}},i:{f:{e:null},h:{g:null}}}", id() );
//            dump();
            string ns = id().indexNamespace();
            ASSERT_EQUALS( 11, bt()->fullValidate( dl(), order(), 0, true ) );
            ASSERT_EQUALS( 7, nsdetails( ns.c_str() )->stats.nrecords );

            BSONObj k = BSON( "" << "bb" );
            verify( unindex( k ) );
//            dump();
            ASSERT_EQUALS( 10, bt()->fullValidate( dl(), order(), 0, true ) );
            // child does not currently replace parent in this case
            ASSERT_EQUALS( 6, nsdetails( ns.c_str() )->stats.nrecords );
            ArtificialTree::checkStructure( "{i:{b:{a:null},cc:{c:null},d:null,f:{e:null},h:{g:null}}}", id() );
        }
    };

    class ParentMergeNonRightToRight : public Base {
    public:
        void run() {
            ArtificialTree::setTree( "{d:{b:{a:null},cc:{c:null}},i:{f:{e:null},ff:null,h:{g:null}}}", id() );
//            dump();
            string ns = id().indexNamespace();
            ASSERT_EQUALS( 11, bt()->fullValidate( dl(), order(), 0, true ) );
            ASSERT_EQUALS( 7, nsdetails( ns.c_str() )->stats.nrecords );

            BSONObj k = BSON( "" << "ff" );
            verify( unindex( k ) );
//            dump();
            ASSERT_EQUALS( 10, bt()->fullValidate( dl(), order(), 0, true ) );
            // child does not currently replace parent in this case
            ASSERT_EQUALS( 6, nsdetails( ns.c_str() )->stats.nrecords );
            ArtificialTree::checkStructure( "{i:{b:{a:null},cc:{c:null},d:null,f:{e:null},h:{g:null}}}", id() );
        }
    };

    class CantMergeRightNoMerge : public Base {
    public:
        void run() {
            ArtificialTree::setTree( "{d:{b:{a:null},bb:null,cc:{c:null}},dd:null,_:{f:{e:null},h:{g:null}}}", id() );
//            dump();
            string ns = id().indexNamespace();
            ASSERT_EQUALS( 11, bt()->fullValidate( dl(), order(), 0, true ) );
            ASSERT_EQUALS( 7, nsdetails( ns.c_str() )->stats.nrecords );

            BSONObj k = BSON( "" << "bb" );
            verify( unindex( k ) );
//            dump();
            ASSERT_EQUALS( 10, bt()->fullValidate( dl(), order(), 0, true ) );
            ASSERT_EQUALS( 7, nsdetails( ns.c_str() )->stats.nrecords );
            ArtificialTree::checkStructure( "{d:{b:{a:null},cc:{c:null}},dd:null,_:{f:{e:null},h:{g:null}}}", id() );
        }
    };

    class CantMergeLeftNoMerge : public Base {
    public:
        void run() {
            ArtificialTree::setTree( "{c:{b:{a:null}},d:null,_:{f:{e:null},g:null}}", id() );
//            dump();
            string ns = id().indexNamespace();
            ASSERT_EQUALS( 7, bt()->fullValidate( dl(), order(), 0, true ) );
            ASSERT_EQUALS( 5, nsdetails( ns.c_str() )->stats.nrecords );

            BSONObj k = BSON( "" << "g" );
            verify( unindex( k ) );
//            dump();
            ASSERT_EQUALS( 6, bt()->fullValidate( dl(), order(), 0, true ) );
            ASSERT_EQUALS( 5, nsdetails( ns.c_str() )->stats.nrecords );
            ArtificialTree::checkStructure( "{c:{b:{a:null}},d:null,_:{f:{e:null}}}", id() );
        }
    };

    class MergeOption : public Base {
    public:
        void run() {
            ArtificialTree::setTree( "{c:{b:{a:null}},f:{e:{d:null},ee:null},_:{h:{g:null}}}", id() );
//            dump();
            string ns = id().indexNamespace();
            ASSERT_EQUALS( 9, bt()->fullValidate( dl(), order(), 0, true ) );
            ASSERT_EQUALS( 7, nsdetails( ns.c_str() )->stats.nrecords );

            BSONObj k = BSON( "" << "ee" );
            verify( unindex( k ) );
//            dump();
            ASSERT_EQUALS( 8, bt()->fullValidate( dl(), order(), 0, true ) );
            ASSERT_EQUALS( 6, nsdetails( ns.c_str() )->stats.nrecords );
            ArtificialTree::checkStructure( "{c:{b:{a:null}},_:{e:{d:null},f:null,h:{g:null}}}", id() );
        }
    };

    class ForceMergeLeft : public Base {
    public:
        void run() {
            ArtificialTree::setTree( "{c:{b:{a:null}},f:{e:{d:null},ee:null},ff:null,_:{h:{g:null}}}", id() );
//            dump();
            string ns = id().indexNamespace();
            ASSERT_EQUALS( 10, bt()->fullValidate( dl(), order(), 0, true ) );
            ASSERT_EQUALS( 7, nsdetails( ns.c_str() )->stats.nrecords );

            BSONObj k = BSON( "" << "ee" );
            verify( unindex( k ) );
//            dump();
            ASSERT_EQUALS( 9, bt()->fullValidate( dl(), order(), 0, true ) );
            ASSERT_EQUALS( 6, nsdetails( ns.c_str() )->stats.nrecords );
            ArtificialTree::checkStructure( "{f:{b:{a:null},c:null,e:{d:null}},ff:null,_:{h:{g:null}}}", id() );
        }
    };

    class ForceMergeRight : public Base {
    public:
        void run() {
            ArtificialTree::setTree( "{c:{b:{a:null}},cc:null,f:{e:{d:null},ee:null},_:{h:{g:null}}}", id() );
//            dump();
            string ns = id().indexNamespace();
            ASSERT_EQUALS( 10, bt()->fullValidate( dl(), order(), 0, true ) );
            ASSERT_EQUALS( 7, nsdetails( ns.c_str() )->stats.nrecords );

            BSONObj k = BSON( "" << "ee" );
            verify( unindex( k ) );
//            dump();
            ASSERT_EQUALS( 9, bt()->fullValidate( dl(), order(), 0, true ) );
            ASSERT_EQUALS( 6, nsdetails( ns.c_str() )->stats.nrecords );
            ArtificialTree::checkStructure( "{c:{b:{a:null}},cc:null,_:{e:{d:null},f:null,h:{g:null}}}", id() );
        }
    };

    class RecursiveMerge : public Base {
    public:
        void run() {
            ArtificialTree::setTree( "{h:{e:{b:{a:null},c:null,d:null},g:{f:null}},j:{i:null}}", id() );
//            dump();
            string ns = id().indexNamespace();
            ASSERT_EQUALS( 10, bt()->fullValidate( dl(), order(), 0, true ) );
            ASSERT_EQUALS( 6, nsdetails( ns.c_str() )->stats.nrecords );

            BSONObj k = BSON( "" << "c" );
            verify( unindex( k ) );
//            dump();
            ASSERT_EQUALS( 9, bt()->fullValidate( dl(), order(), 0, true ) );
            ASSERT_EQUALS( 4, nsdetails( ns.c_str() )->stats.nrecords );
            // height is not currently reduced in this case
            ArtificialTree::checkStructure( "{j:{g:{b:{a:null},d:null,e:null,f:null},h:null,i:null}}", id() );
        }
    };

    class RecursiveMergeRightBucket : public Base {
    public:
        void run() {
            ArtificialTree::setTree( "{h:{e:{b:{a:null},c:null,d:null},g:{f:null}},_:{i:null}}", id() );
//            dump();
            string ns = id().indexNamespace();
            ASSERT_EQUALS( 9, bt()->fullValidate( dl(), order(), 0, true ) );
            ASSERT_EQUALS( 6, nsdetails( ns.c_str() )->stats.nrecords );

            BSONObj k = BSON( "" << "c" );
            verify( unindex( k ) );
//            dump();
            ASSERT_EQUALS( 8, bt()->fullValidate( dl(), order(), 0, true ) );
            ASSERT_EQUALS( 3, nsdetails( ns.c_str() )->stats.nrecords );
            ArtificialTree::checkStructure( "{g:{b:{a:null},d:null,e:null,f:null},h:null,i:null}", id() );
        }
    };

    class RecursiveMergeDoubleRightBucket : public Base {
    public:
        void run() {
            ArtificialTree::setTree( "{h:{e:{b:{a:null},c:null,d:null},_:{f:null}},_:{i:null}}", id() );
            string ns = id().indexNamespace();
            ASSERT_EQUALS( 8, bt()->fullValidate( dl(), order(), 0, true ) );
            ASSERT_EQUALS( 6, nsdetails( ns.c_str() )->stats.nrecords );
            BSONObj k = BSON( "" << "c" );
            verify( unindex( k ) );
            long long keyCount = bt()->fullValidate( dl(), order(), 0, true );
            ASSERT_EQUALS( 7, keyCount );
            ASSERT_EQUALS( 4, nsdetails( ns.c_str() )->stats.nrecords );
            // no recursion currently in this case
            ArtificialTree::checkStructure( "{h:{b:{a:null},d:null,e:null,f:null},_:{i:null}}", id() );
        }
    };

    class MergeSizeBase : public Base {
    public:
        MergeSizeBase() : _count() {}
        virtual ~MergeSizeBase() {}
        void run() {
            typedef ArtificialTree A;
            A::set( A::make( id() ), id() );
            A* root = A::is( dl() );
            DiskLoc left = A::make( id() );
            root->push( biggestKey( 'm' ), left );
            _count = 1;
            A* l = A::is( left );
            DiskLoc right = A::make( id() );
            root->setNext( right );
            A* r = A::is( right );
            root->fixParentPtrs( dl() );

            //ASSERT_EQUALS( bigSize(), bigSize() / 2 * 2 );
            fillToExactSize( l, leftSize(), 'a' );
            fillToExactSize( r, rightSize(), 'n' );
            ASSERT( leftAdditional() <= 2 );
            if ( leftAdditional() >= 2 ) {
                l->push( bigKey( 'k' ), DiskLoc() );
            }
            if ( leftAdditional() >= 1 ) {
                l->push( bigKey( 'l' ), DiskLoc() );
            }
            ASSERT( rightAdditional() <= 2 );
            if ( rightAdditional() >= 2 ) {
                r->push( bigKey( 'y' ), DiskLoc() );
            }
            if ( rightAdditional() >= 1 ) {
                r->push( bigKey( 'z' ), DiskLoc() );
            }
            _count += leftAdditional() + rightAdditional();

//            dump();

            initCheck();
            string ns = id().indexNamespace();
            const char *keys = delKeys();
            for( const char *i = keys; *i; ++i ) {
                long long unused = 0;
                ASSERT_EQUALS( _count, bt()->fullValidate( dl(), order(), &unused, true ) );
                ASSERT_EQUALS( 0, unused );
                ASSERT_EQUALS( 3, nsdetails( ns.c_str() )->stats.nrecords );
                BSONObj k = bigKey( *i );
                unindex( k );
//                dump();
                --_count;
            }

//            dump();

            long long unused = 0;
            ASSERT_EQUALS( _count, bt()->fullValidate( dl(), order(), &unused, true ) );
            ASSERT_EQUALS( 0, unused );
            validate();
            if ( !merge() ) {
                ASSERT_EQUALS( 3, nsdetails( ns.c_str() )->stats.nrecords );
            }
            else {
                ASSERT_EQUALS( 1, nsdetails( ns.c_str() )->stats.nrecords );
            }
        }
    protected:
        virtual int leftAdditional() const { return 2; }
        virtual int rightAdditional() const { return 2; }
        virtual void initCheck() {}
        virtual void validate() {}
        virtual int leftSize() const = 0;
        virtual int rightSize() const = 0;
        virtual const char * delKeys() const { return "klyz"; }
        virtual bool merge() const { return true; }
        void fillToExactSize( ArtificialTree *t, int targetSize, char startKey ) {
            int size = 0;
            while( size < targetSize ) {
                int space = targetSize - size;
                int nextSize = space - sizeof( _KeyNode );
                verify( nextSize > 0 );
                BSONObj newKey = key( startKey++, nextSize );
                t->push( newKey, DiskLoc() );
                size += BtreeBucket::KeyOwned(newKey).dataSize() + sizeof( _KeyNode );
                _count += 1;
            }
            if( t->packedDataSize( 0 ) != targetSize ) {
                ASSERT_EQUALS( t->packedDataSize( 0 ), targetSize );
            }
        }
        static BSONObj key( char a, int size ) {
            if ( size >= bigSize() ) {
                return bigKey( a );
            }
            return simpleKey( a, size - ( bigSize() - 801 ) );
        }
        static BSONObj bigKey( char a ) {
            return simpleKey( a, 801 );
        }
        static BSONObj biggestKey( char a ) {
            int size = BtreeBucket::getKeyMax() - bigSize() + 801;
            return simpleKey( a, size );
        }
        static int bigSize() {
            return BtreeBucket::KeyOwned(bigKey( 'a' )).dataSize();
        }
        static int biggestSize() {
            return BtreeBucket::KeyOwned(biggestKey( 'a' )).dataSize();
        }
        int _count;
    };

    class MergeSizeJustRightRight : public MergeSizeBase {
    protected:
        virtual int rightSize() const { return BtreeBucket::lowWaterMark() - 1; }
        virtual int leftSize() const { return BtreeBucket::bodySize() - biggestSize() - sizeof( _KeyNode ) - ( BtreeBucket::lowWaterMark() - 1 ); }
    };

    class MergeSizeJustRightLeft : public MergeSizeBase {
    protected:
        virtual int leftSize() const { return BtreeBucket::lowWaterMark() - 1; }
        virtual int rightSize() const { return BtreeBucket::bodySize() - biggestSize() - sizeof( _KeyNode ) - ( BtreeBucket::lowWaterMark() - 1 ); }
        virtual const char * delKeys() const { return "yzkl"; }
    };

    class MergeSizeRight : public MergeSizeJustRightRight {
        virtual int rightSize() const { return MergeSizeJustRightRight::rightSize() - 1; }
        virtual int leftSize() const { return MergeSizeJustRightRight::leftSize() + 1; }
    };

    class MergeSizeLeft : public MergeSizeJustRightLeft {
        virtual int rightSize() const { return MergeSizeJustRightLeft::rightSize() + 1; }
        virtual int leftSize() const { return MergeSizeJustRightLeft::leftSize() - 1; }
    };

    class NoMergeBelowMarkRight : public MergeSizeJustRightRight {
        virtual int rightSize() const { return MergeSizeJustRightRight::rightSize() + 1; }
        virtual int leftSize() const { return MergeSizeJustRightRight::leftSize() - 1; }
        virtual bool merge() const { return false; }
    };

    class NoMergeBelowMarkLeft : public MergeSizeJustRightLeft {
        virtual int rightSize() const { return MergeSizeJustRightLeft::rightSize() - 1; }
        virtual int leftSize() const { return MergeSizeJustRightLeft::leftSize() + 1; }
        virtual bool merge() const { return false; }
    };

    class MergeSizeRightTooBig : public MergeSizeJustRightLeft {
        virtual int rightSize() const { return MergeSizeJustRightLeft::rightSize() + 1; }
        virtual bool merge() const { return false; }
    };

    class MergeSizeLeftTooBig : public MergeSizeJustRightRight {
        virtual int leftSize() const { return MergeSizeJustRightRight::leftSize() + 1; }
        virtual bool merge() const { return false; }
    };

    class BalanceOneLeftToRight : public Base {
    public:
        void run() {
            string ns = id().indexNamespace();
            ArtificialTree::setTree( "{$10:{$1:null,$2:null,$3:null,$4:null,$5:null,$6:null},b:{$20:null,$30:null,$40:null,$50:null,a:null},_:{c:null}}", id() );
            ASSERT_EQUALS( 14, bt()->fullValidate( dl(), order(), 0, true ) );
            ASSERT_EQUALS( 4, nsdetails( ns.c_str() )->stats.nrecords );
            BSONObj k = BSON( "" << bigNumString( 0x40 ) );
//            dump();
            ASSERT( unindex( k ) );
//            dump();
            ASSERT_EQUALS( 13, bt()->fullValidate( dl(), order(), 0, true ) );
            ASSERT_EQUALS( 4, nsdetails( ns.c_str() )->stats.nrecords );
            ArtificialTree::checkStructure( "{$6:{$1:null,$2:null,$3:null,$4:null,$5:null},b:{$10:null,$20:null,$30:null,$50:null,a:null},_:{c:null}}", id() );
        }
    };

    class BalanceOneRightToLeft : public Base {
    public:
        void run() {
            string ns = id().indexNamespace();
            ArtificialTree::setTree( "{$10:{$1:null,$2:null,$3:null,$4:null},b:{$20:null,$30:null,$40:null,$50:null,$60:null,$70:null},_:{c:null}}", id() );
            ASSERT_EQUALS( 13, bt()->fullValidate( dl(), order(), 0, true ) );
            ASSERT_EQUALS( 4, nsdetails( ns.c_str() )->stats.nrecords );
            BSONObj k = BSON( "" << bigNumString( 0x3 ) );
//            dump();
            ASSERT( unindex( k ) );
//            dump();
            ASSERT_EQUALS( 12, bt()->fullValidate( dl(), order(), 0, true ) );
            ASSERT_EQUALS( 4, nsdetails( ns.c_str() )->stats.nrecords );
            ArtificialTree::checkStructure( "{$20:{$1:null,$2:null,$4:null,$10:null},b:{$30:null,$40:null,$50:null,$60:null,$70:null},_:{c:null}}", id() );
        }
    };

    class BalanceThreeLeftToRight : public Base {
    public:
        void run() {
            string ns = id().indexNamespace();
            ArtificialTree::setTree( "{$20:{$1:{$0:null},$3:{$2:null},$5:{$4:null},$7:{$6:null},$9:{$8:null},$11:{$10:null},$13:{$12:null},_:{$14:null}},b:{$30:null,$40:{$35:null},$50:{$45:null}},_:{c:null}}", id() );
            ASSERT_EQUALS( 23, bt()->fullValidate( dl(), order(), 0, true ) );
            ASSERT_EQUALS( 14, nsdetails( ns.c_str() )->stats.nrecords );
            BSONObj k = BSON( "" << bigNumString( 0x30 ) );
            //            dump();
            ASSERT( unindex( k ) );
            //            dump();
            ASSERT_EQUALS( 22, bt()->fullValidate( dl(), order(), 0, true ) );
            ASSERT_EQUALS( 14, nsdetails( ns.c_str() )->stats.nrecords );
            ArtificialTree::checkStructure( "{$9:{$1:{$0:null},$3:{$2:null},$5:{$4:null},$7:{$6:null},_:{$8:null}},b:{$11:{$10:null},$13:{$12:null},$20:{$14:null},$40:{$35:null},$50:{$45:null}},_:{c:null}}", id() );
        }
    };

    class BalanceThreeRightToLeft : public Base {
    public:
        void run() {
            string ns = id().indexNamespace();
            ArtificialTree::setTree( "{$20:{$1:{$0:null},$3:{$2:null},$5:null,_:{$14:null}},b:{$30:{$25:null},$40:{$35:null},$50:{$45:null},$60:{$55:null},$70:{$65:null},$80:{$75:null},$90:{$85:null},$100:{$95:null}},_:{c:null}}", id() );
            ASSERT_EQUALS( 25, bt()->fullValidate( dl(), order(), 0, true ) );
            ASSERT_EQUALS( 15, nsdetails( ns.c_str() )->stats.nrecords );
            BSONObj k = BSON( "" << bigNumString( 0x5 ) );
//            dump();
            ASSERT( unindex( k ) );
//            dump();
            ASSERT_EQUALS( 24, bt()->fullValidate( dl(), order(), 0, true ) );
            ASSERT_EQUALS( 15, nsdetails( ns.c_str() )->stats.nrecords );
            ArtificialTree::checkStructure( "{$50:{$1:{$0:null},$3:{$2:null},$20:{$14:null},$30:{$25:null},$40:{$35:null},_:{$45:null}},b:{$60:{$55:null},$70:{$65:null},$80:{$75:null},$90:{$85:null},$100:{$95:null}},_:{c:null}}", id() );
        }
    };

    class BalanceSingleParentKey : public Base {
    public:
        void run() {
            string ns = id().indexNamespace();
            ArtificialTree::setTree( "{$10:{$1:null,$2:null,$3:null,$4:null,$5:null,$6:null},_:{$20:null,$30:null,$40:null,$50:null,a:null}}", id() );
            ASSERT_EQUALS( 12, bt()->fullValidate( dl(), order(), 0, true ) );
            ASSERT_EQUALS( 3, nsdetails( ns.c_str() )->stats.nrecords );
            BSONObj k = BSON( "" << bigNumString( 0x40 ) );
//            dump();
            ASSERT( unindex( k ) );
//            dump();
            ASSERT_EQUALS( 11, bt()->fullValidate( dl(), order(), 0, true ) );
            ASSERT_EQUALS( 3, nsdetails( ns.c_str() )->stats.nrecords );
            ArtificialTree::checkStructure( "{$6:{$1:null,$2:null,$3:null,$4:null,$5:null},_:{$10:null,$20:null,$30:null,$50:null,a:null}}", id() );
        }
    };

    class PackEmpty : public Base {
    public:
        void run() {
            string ns = id().indexNamespace();
            ArtificialTree::setTree( "{a:null}", id() );
            BSONObj k = BSON( "" << "a" );
            ASSERT( unindex( k ) );
            ArtificialTree *t = ArtificialTree::is( dl() );
            t->forcePack();
            Tester::checkEmpty( t, id() );
        }
        class Tester : public ArtificialTree {
        public:
            static void checkEmpty( ArtificialTree *a, const IndexDetails &id ) {
                Tester *t = static_cast< Tester * >( a );
                ASSERT_EQUALS( 0, t->n );
                ASSERT( !( t->flags & Packed ) );
                Ordering o = Ordering::make( id.keyPattern() );
                int zero = 0;
                t->_packReadyForMod( o, zero );
                ASSERT_EQUALS( 0, t->n );
                ASSERT_EQUALS( 0, t->topSize );
                ASSERT_EQUALS( BtreeBucket::bodySize(), t->emptySize );
                ASSERT( t->flags & Packed );
            }
        };
    };

    class PackedDataSizeEmpty : public Base {
    public:
        void run() {
            string ns = id().indexNamespace();
            ArtificialTree::setTree( "{a:null}", id() );
            BSONObj k = BSON( "" << "a" );
            ASSERT( unindex( k ) );
            ArtificialTree *t = ArtificialTree::is( dl() );
            t->forcePack();
            Tester::checkEmpty( t, id() );
        }
        class Tester : public ArtificialTree {
        public:
            static void checkEmpty( ArtificialTree *a, const IndexDetails &id ) {
                Tester *t = static_cast< Tester * >( a );
                ASSERT_EQUALS( 0, t->n );
                ASSERT( !( t->flags & Packed ) );
                int zero = 0;
                ASSERT_EQUALS( 0, t->packedDataSize( zero ) );
                ASSERT( !( t->flags & Packed ) );
            }
        };
    };

    class BalanceSingleParentKeyPackParent : public Base {
    public:
        void run() {
            string ns = id().indexNamespace();
            ArtificialTree::setTree( "{$10:{$1:null,$2:null,$3:null,$4:null,$5:null,$6:null},_:{$20:null,$30:null,$40:null,$50:null,a:null}}", id() );
            ASSERT_EQUALS( 12, bt()->fullValidate( dl(), order(), 0, true ) );
            ASSERT_EQUALS( 3, nsdetails( ns.c_str() )->stats.nrecords );
            // force parent pack
            ArtificialTree::is( dl() )->forcePack();
            BSONObj k = BSON( "" << bigNumString( 0x40 ) );
//            dump();
            ASSERT( unindex( k ) );
//            dump();
            ASSERT_EQUALS( 11, bt()->fullValidate( dl(), order(), 0, true ) );
            ASSERT_EQUALS( 3, nsdetails( ns.c_str() )->stats.nrecords );
            ArtificialTree::checkStructure( "{$6:{$1:null,$2:null,$3:null,$4:null,$5:null},_:{$10:null,$20:null,$30:null,$50:null,a:null}}", id() );
        }
    };

    class BalanceSplitParent : public Base {
    public:
        void run() {
            string ns = id().indexNamespace();
            ArtificialTree::setTree( "{$10$10:{$1:null,$2:null,$3:null,$4:null},$100:{$20:null,$30:null,$40:null,$50:null,$60:null,$70:null,$80:null},$200:null,$300:null,$400:null,$500:null,$600:null,$700:null,$800:null,$900:null,_:{c:null}}", id() );
            ASSERT_EQUALS( 22, bt()->fullValidate( dl(), order(), 0, true ) );
            ASSERT_EQUALS( 4, nsdetails( ns.c_str() )->stats.nrecords );
            BSONObj k = BSON( "" << bigNumString( 0x3 ) );
//            dump();
            ASSERT( unindex( k ) );
//            dump();
            ASSERT_EQUALS( 21, bt()->fullValidate( dl(), order(), 0, true ) );
            ASSERT_EQUALS( 6, nsdetails( ns.c_str() )->stats.nrecords );
            ArtificialTree::checkStructure( "{$500:{$30:{$1:null,$2:null,$4:null,$10$10:null,$20:null},$100:{$40:null,$50:null,$60:null,$70:null,$80:null},$200:null,$300:null,$400:null},_:{$600:null,$700:null,$800:null,$900:null,_:{c:null}}}", id() );
        }
    };

    class RebalancedSeparatorBase : public Base {
    public:
        void run() {
            ArtificialTree::setTree( treeSpec(), id() );
            modTree();
            Tester::checkSeparator( id(), expectedSeparator() );
        }
        virtual string treeSpec() const = 0;
        virtual int expectedSeparator() const = 0;
        virtual void modTree() {}
        struct Tester : public ArtificialTree {
            static void checkSeparator( const IndexDetails& id, int expected ) {
                ASSERT_EQUALS( expected, static_cast< Tester * >( id.head.btreemod() )->rebalancedSeparatorPos( id.head, 0 ) );
            }
        };
    };

    class EvenRebalanceLeft : public RebalancedSeparatorBase {
        virtual string treeSpec() const { return "{$7:{$1:null,$2$31f:null,$3:null,$4$31f:null,$5:null,$6:null},_:{$8:null,$9:null,$10$31e:null}}"; }
        virtual int expectedSeparator() const { return 4; }
    };

    class EvenRebalanceLeftCusp : public RebalancedSeparatorBase {
        virtual string treeSpec() const { return "{$6:{$1:null,$2$31f:null,$3:null,$4$31f:null,$5:null},_:{$7:null,$8:null,$9$31e:null,$10:null}}"; }
        virtual int expectedSeparator() const { return 4; }
    };

    class EvenRebalanceRight : public RebalancedSeparatorBase {
        virtual string treeSpec() const { return "{$3:{$1:null,$2$31f:null},_:{$4$31f:null,$5:null,$6:null,$7:null,$8$31e:null,$9:null,$10:null}}"; }
        virtual int expectedSeparator() const { return 4; }
    };

    class EvenRebalanceRightCusp : public RebalancedSeparatorBase {
        virtual string treeSpec() const { return "{$4$31f:{$1:null,$2$31f:null,$3:null},_:{$5:null,$6:null,$7$31e:null,$8:null,$9:null,$10:null}}"; }
        virtual int expectedSeparator() const { return 4; }
    };

    class EvenRebalanceCenter : public RebalancedSeparatorBase {
        virtual string treeSpec() const { return "{$5:{$1:null,$2$31f:null,$3:null,$4$31f:null},_:{$6:null,$7$31e:null,$8:null,$9:null,$10:null}}"; }
        virtual int expectedSeparator() const { return 4; }
    };

    class OddRebalanceLeft : public RebalancedSeparatorBase {
        virtual string treeSpec() const { return "{$6$31f:{$1:null,$2:null,$3:null,$4:null,$5:null},_:{$7:null,$8:null,$9:null,$10:null}}"; }
        virtual int expectedSeparator() const { return 4; }
    };

    class OddRebalanceRight : public RebalancedSeparatorBase {
        virtual string treeSpec() const { return "{$4:{$1:null,$2:null,$3:null},_:{$5:null,$6:null,$7:null,$8$31f:null,$9:null,$10:null}}"; }
        virtual int expectedSeparator() const { return 4; }
    };

    class OddRebalanceCenter : public RebalancedSeparatorBase {
        virtual string treeSpec() const { return "{$5:{$1:null,$2:null,$3:null,$4:null},_:{$6:null,$7:null,$8:null,$9:null,$10$31f:null}}"; }
        virtual int expectedSeparator() const { return 4; }
    };

    class RebalanceEmptyRight : public RebalancedSeparatorBase {
        virtual string treeSpec() const { return "{$a:{$1:null,$2:null,$3:null,$4:null,$5:null,$6:null,$7:null,$8:null,$9:null},_:{$b:null}}"; }
        virtual void modTree() {
            BSONObj k = BSON( "" << bigNumString( 0xb ) );
            ASSERT( unindex( k ) );
        }
        virtual int expectedSeparator() const { return 4; }
    };

    class RebalanceEmptyLeft : public RebalancedSeparatorBase {
        virtual string treeSpec() const { return "{$a:{$1:null},_:{$11:null,$12:null,$13:null,$14:null,$15:null,$16:null,$17:null,$18:null,$19:null}}"; }
        virtual void modTree() {
            BSONObj k = BSON( "" << bigNumString( 0x1 ) );
            ASSERT( unindex( k ) );
        }
        virtual int expectedSeparator() const { return 4; }
    };

    class NoMoveAtLowWaterMarkRight : public MergeSizeJustRightRight {
        virtual int rightSize() const { return MergeSizeJustRightRight::rightSize() + 1; }
        virtual void initCheck() { _oldTop = bt()->keyNode( 0 ).key.toBson(); }
        virtual void validate() { ASSERT_EQUALS( _oldTop, bt()->keyNode( 0 ).key.toBson() ); }
        virtual bool merge() const { return false; }
    protected:
        BSONObj _oldTop;
    };

    class MoveBelowLowWaterMarkRight : public NoMoveAtLowWaterMarkRight {
        virtual int rightSize() const { return MergeSizeJustRightRight::rightSize(); }
        virtual int leftSize() const { return MergeSizeJustRightRight::leftSize() + 1; }
        // different top means we rebalanced
        virtual void validate() { ASSERT( !( _oldTop == bt()->keyNode( 0 ).key.toBson() ) ); }
    };

    class NoMoveAtLowWaterMarkLeft : public MergeSizeJustRightLeft {
        virtual int leftSize() const { return MergeSizeJustRightLeft::leftSize() + 1; }
        virtual void initCheck() { _oldTop = bt()->keyNode( 0 ).key.toBson(); }
        virtual void validate() { ASSERT_EQUALS( _oldTop, bt()->keyNode( 0 ).key.toBson() ); }
        virtual bool merge() const { return false; }
    protected:
        BSONObj _oldTop;
    };

    class MoveBelowLowWaterMarkLeft : public NoMoveAtLowWaterMarkLeft {
        virtual int leftSize() const { return MergeSizeJustRightLeft::leftSize(); }
        virtual int rightSize() const { return MergeSizeJustRightLeft::rightSize() + 1; }
        // different top means we rebalanced
        virtual void validate() { ASSERT( !( _oldTop == bt()->keyNode( 0 ).key.toBson() ) ); }
    };

    class PreferBalanceLeft : public Base {
    public:
        void run() {
            string ns = id().indexNamespace();
            ArtificialTree::setTree( "{$10:{$1:null,$2:null,$3:null,$4:null,$5:null,$6:null},$20:{$11:null,$12:null,$13:null,$14:null},_:{$30:null}}", id() );
            ASSERT_EQUALS( 13, bt()->fullValidate( dl(), order(), 0, true ) );
            ASSERT_EQUALS( 4, nsdetails( ns.c_str() )->stats.nrecords );
            BSONObj k = BSON( "" << bigNumString( 0x12 ) );
//            dump();
            ASSERT( unindex( k ) );
//            dump();
            ASSERT_EQUALS( 12, bt()->fullValidate( dl(), order(), 0, true ) );
            ASSERT_EQUALS( 4, nsdetails( ns.c_str() )->stats.nrecords );
            ArtificialTree::checkStructure( "{$5:{$1:null,$2:null,$3:null,$4:null},$20:{$6:null,$10:null,$11:null,$13:null,$14:null},_:{$30:null}}", id() );
        }
    };

    class PreferBalanceRight : public Base {
    public:
        void run() {
            string ns = id().indexNamespace();
            ArtificialTree::setTree( "{$10:{$1:null},$20:{$11:null,$12:null,$13:null,$14:null},_:{$31:null,$32:null,$33:null,$34:null,$35:null,$36:null}}", id() );
            ASSERT_EQUALS( 13, bt()->fullValidate( dl(), order(), 0, true ) );
            ASSERT_EQUALS( 4, nsdetails( ns.c_str() )->stats.nrecords );
            BSONObj k = BSON( "" << bigNumString( 0x12 ) );
            //            dump();
            ASSERT( unindex( k ) );
            //            dump();
            ASSERT_EQUALS( 12, bt()->fullValidate( dl(), order(), 0, true ) );
            ASSERT_EQUALS( 4, nsdetails( ns.c_str() )->stats.nrecords );
            ArtificialTree::checkStructure( "{$10:{$1:null},$31:{$11:null,$13:null,$14:null,$20:null},_:{$32:null,$33:null,$34:null,$35:null,$36:null}}", id() );
        }
    };

    class RecursiveMergeThenBalance : public Base {
    public:
        void run() {
            string ns = id().indexNamespace();
            ArtificialTree::setTree( "{$10:{$5:{$1:null,$2:null},$8:{$6:null,$7:null}},_:{$20:null,$30:null,$40:null,$50:null,$60:null,$70:null,$80:null,$90:null}}", id() );
            ASSERT_EQUALS( 15, bt()->fullValidate( dl(), order(), 0, true ) );
            ASSERT_EQUALS( 5, nsdetails( ns.c_str() )->stats.nrecords );
            BSONObj k = BSON( "" << bigNumString( 0x7 ) );
            //            dump();
            ASSERT( unindex( k ) );
            //            dump();
            ASSERT_EQUALS( 14, bt()->fullValidate( dl(), order(), 0, true ) );
            ASSERT_EQUALS( 4, nsdetails( ns.c_str() )->stats.nrecords );
            ArtificialTree::checkStructure( "{$40:{$8:{$1:null,$2:null,$5:null,$6:null},$10:null,$20:null,$30:null},_:{$50:null,$60:null,$70:null,$80:null,$90:null}}", id() );
        }
    };

    class MergeRightEmpty : public MergeSizeBase {
    protected:
        virtual int rightAdditional() const { return 1; }
        virtual int leftAdditional() const { return 1; }
        virtual const char * delKeys() const { return "lz"; }
        virtual int rightSize() const { return 0; }
        virtual int leftSize() const { return BtreeBucket::bodySize() - biggestSize() - sizeof( _KeyNode ); }
    };

    class MergeMinRightEmpty : public MergeSizeBase {
    protected:
        virtual int rightAdditional() const { return 1; }
        virtual int leftAdditional() const { return 0; }
        virtual const char * delKeys() const { return "z"; }
        virtual int rightSize() const { return 0; }
        virtual int leftSize() const { return bigSize() + sizeof( _KeyNode ); }
    };

    class MergeLeftEmpty : public MergeSizeBase {
    protected:
        virtual int rightAdditional() const { return 1; }
        virtual int leftAdditional() const { return 1; }
        virtual const char * delKeys() const { return "zl"; }
        virtual int leftSize() const { return 0; }
        virtual int rightSize() const { return BtreeBucket::bodySize() - biggestSize() - sizeof( _KeyNode ); }
    };

    class MergeMinLeftEmpty : public MergeSizeBase {
    protected:
        virtual int leftAdditional() const { return 1; }
        virtual int rightAdditional() const { return 0; }
        virtual const char * delKeys() const { return "l"; }
        virtual int leftSize() const { return 0; }
        virtual int rightSize() const { return bigSize() + sizeof( _KeyNode ); }
    };

    class BalanceRightEmpty : public MergeRightEmpty {
    protected:
        virtual int leftSize() const { return BtreeBucket::bodySize() - biggestSize() - sizeof( _KeyNode ) + 1; }
        virtual bool merge() const { return false; }
        virtual void initCheck() { _oldTop = bt()->keyNode( 0 ).key.toBson(); }
        virtual void validate() { ASSERT( !( _oldTop == bt()->keyNode( 0 ).key.toBson() ) ); }
    private:
        BSONObj _oldTop;
    };

    class BalanceLeftEmpty : public MergeLeftEmpty {
    protected:
        virtual int rightSize() const { return BtreeBucket::bodySize() - biggestSize() - sizeof( _KeyNode ) + 1; }
        virtual bool merge() const { return false; }
        virtual void initCheck() { _oldTop = bt()->keyNode( 0 ).key.toBson(); }
        virtual void validate() { ASSERT( !( _oldTop == bt()->keyNode( 0 ).key.toBson() ) ); }
    private:
        BSONObj _oldTop;
    };

    class DelEmptyNoNeighbors : public Base {
    public:
        void run() {
            string ns = id().indexNamespace();
            ArtificialTree::setTree( "{b:{a:null}}", id() );
            ASSERT_EQUALS( 2, bt()->fullValidate( dl(), order(), 0, true ) );
            ASSERT_EQUALS( 2, nsdetails( ns.c_str() )->stats.nrecords );
            BSONObj k = BSON( "" << "a" );
            //            dump();
            ASSERT( unindex( k ) );
            //            dump();
            ASSERT_EQUALS( 1, bt()->fullValidate( dl(), order(), 0, true ) );
            ASSERT_EQUALS( 1, nsdetails( ns.c_str() )->stats.nrecords );
            ArtificialTree::checkStructure( "{b:null}", id() );
        }
    };

    class DelEmptyEmptyNeighbors : public Base {
    public:
        void run() {
            string ns = id().indexNamespace();
            ArtificialTree::setTree( "{a:null,c:{b:null},d:null}", id() );
            ASSERT_EQUALS( 4, bt()->fullValidate( dl(), order(), 0, true ) );
            ASSERT_EQUALS( 2, nsdetails( ns.c_str() )->stats.nrecords );
            BSONObj k = BSON( "" << "b" );
            //            dump();
            ASSERT( unindex( k ) );
            //            dump();
            ASSERT_EQUALS( 3, bt()->fullValidate( dl(), order(), 0, true ) );
            ASSERT_EQUALS( 1, nsdetails( ns.c_str() )->stats.nrecords );
            ArtificialTree::checkStructure( "{a:null,c:null,d:null}", id() );
        }
    };

    class DelInternal : public Base {
    public:
        void run() {
            string ns = id().indexNamespace();
            ArtificialTree::setTree( "{a:null,c:{b:null},d:null}", id() );
            long long unused = 0;
            ASSERT_EQUALS( 4, bt()->fullValidate( dl(), order(), &unused, true ) );
            ASSERT_EQUALS( 0, unused );
            ASSERT_EQUALS( 2, nsdetails( ns.c_str() )->stats.nrecords );
            BSONObj k = BSON( "" << "c" );
//            dump();
            ASSERT( unindex( k ) );
//            dump();
            ASSERT_EQUALS( 3, bt()->fullValidate( dl(), order(), &unused, true ) );
            ASSERT_EQUALS( 0, unused );
            ASSERT_EQUALS( 1, nsdetails( ns.c_str() )->stats.nrecords );
            ArtificialTree::checkStructure( "{a:null,b:null,d:null}", id() );
        }
    };

    class DelInternalReplaceWithUnused : public Base {
    public:
        void run() {
            string ns = id().indexNamespace();
            ArtificialTree::setTree( "{a:null,c:{b:null},d:null}", id() );
            getDur().writingInt( const_cast< BtreeBucket::Loc& >( bt()->keyNode( 1 ).prevChildBucket.btree()->keyNode( 0 ).recordLoc ).GETOFS() ) |= 1; // make unused
            long long unused = 0;
            ASSERT_EQUALS( 3, bt()->fullValidate( dl(), order(), &unused, true ) );
            ASSERT_EQUALS( 1, unused );
            ASSERT_EQUALS( 2, nsdetails( ns.c_str() )->stats.nrecords );
            BSONObj k = BSON( "" << "c" );
//            dump();
            ASSERT( unindex( k ) );
//            dump();
            unused = 0;
            ASSERT_EQUALS( 2, bt()->fullValidate( dl(), order(), &unused, true ) );
            ASSERT_EQUALS( 1, unused );
            ASSERT_EQUALS( 1, nsdetails( ns.c_str() )->stats.nrecords );
            // doesn't discriminate between used and unused
            ArtificialTree::checkStructure( "{a:null,b:null,d:null}", id() );
        }
    };

    class DelInternalReplaceRight : public Base {
    public:
        void run() {
            string ns = id().indexNamespace();
            ArtificialTree::setTree( "{a:null,_:{b:null}}", id() );
            long long unused = 0;
            ASSERT_EQUALS( 2, bt()->fullValidate( dl(), order(), &unused, true ) );
            ASSERT_EQUALS( 0, unused );
            ASSERT_EQUALS( 2, nsdetails( ns.c_str() )->stats.nrecords );
            BSONObj k = BSON( "" << "a" );
//            dump();
            ASSERT( unindex( k ) );
//            dump();
            unused = 0;
            ASSERT_EQUALS( 1, bt()->fullValidate( dl(), order(), &unused, true ) );
            ASSERT_EQUALS( 0, unused );
            ASSERT_EQUALS( 1, nsdetails( ns.c_str() )->stats.nrecords );
            ArtificialTree::checkStructure( "{b:null}", id() );
        }
    };

    class DelInternalPromoteKey : public Base {
    public:
        void run() {
            string ns = id().indexNamespace();
            ArtificialTree::setTree( "{a:null,y:{d:{c:{b:null}},_:{e:null}},z:null}", id() );
            long long unused = 0;
            ASSERT_EQUALS( 7, bt()->fullValidate( dl(), order(), &unused, true ) );
            ASSERT_EQUALS( 0, unused );
            ASSERT_EQUALS( 5, nsdetails( ns.c_str() )->stats.nrecords );
            BSONObj k = BSON( "" << "y" );
//            dump();
            ASSERT( unindex( k ) );
//            dump();
            unused = 0;
            ASSERT_EQUALS( 6, bt()->fullValidate( dl(), order(), &unused, true ) );
            ASSERT_EQUALS( 0, unused );
            ASSERT_EQUALS( 3, nsdetails( ns.c_str() )->stats.nrecords );
            ArtificialTree::checkStructure( "{a:null,e:{c:{b:null},d:null},z:null}", id() );
        }
    };

    class DelInternalPromoteRightKey : public Base {
    public:
        void run() {
            string ns = id().indexNamespace();
            ArtificialTree::setTree( "{a:null,_:{e:{c:null},_:{f:null}}}", id() );
            long long unused = 0;
            ASSERT_EQUALS( 4, bt()->fullValidate( dl(), order(), &unused, true ) );
            ASSERT_EQUALS( 0, unused );
            ASSERT_EQUALS( 4, nsdetails( ns.c_str() )->stats.nrecords );
            BSONObj k = BSON( "" << "a" );
//            dump();
            ASSERT( unindex( k ) );
//            dump();
            unused = 0;
            ASSERT_EQUALS( 3, bt()->fullValidate( dl(), order(), &unused, true ) );
            ASSERT_EQUALS( 0, unused );
            ASSERT_EQUALS( 2, nsdetails( ns.c_str() )->stats.nrecords );
            ArtificialTree::checkStructure( "{c:null,_:{e:null,f:null}}", id() );
        }
    };

    class DelInternalReplacementPrevNonNull : public Base {
    public:
        void run() {
            string ns = id().indexNamespace();
            ArtificialTree::setTree( "{a:null,d:{c:{b:null}},e:null}", id() );
            long long unused = 0;
            ASSERT_EQUALS( 5, bt()->fullValidate( dl(), order(), &unused, true ) );
            ASSERT_EQUALS( 0, unused );
            ASSERT_EQUALS( 3, nsdetails( ns.c_str() )->stats.nrecords );
            BSONObj k = BSON( "" << "d" );
            //            dump();
            ASSERT( unindex( k ) );
            //            dump();
            ASSERT_EQUALS( 4, bt()->fullValidate( dl(), order(), &unused, true ) );
            ASSERT_EQUALS( 1, unused );
            ASSERT_EQUALS( 3, nsdetails( ns.c_str() )->stats.nrecords );
            ArtificialTree::checkStructure( "{a:null,d:{c:{b:null}},e:null}", id() );
            ASSERT( bt()->keyNode( 1 ).recordLoc.getOfs() & 1 ); // check 'unused' key
        }
    };

    class DelInternalReplacementNextNonNull : public Base {
    public:
        void run() {
            string ns = id().indexNamespace();
            ArtificialTree::setTree( "{a:null,_:{c:null,_:{d:null}}}", id() );
            long long unused = 0;
            ASSERT_EQUALS( 3, bt()->fullValidate( dl(), order(), &unused, true ) );
            ASSERT_EQUALS( 0, unused );
            ASSERT_EQUALS( 3, nsdetails( ns.c_str() )->stats.nrecords );
            BSONObj k = BSON( "" << "a" );
            //            dump();
            ASSERT( unindex( k ) );
            //            dump();
            ASSERT_EQUALS( 2, bt()->fullValidate( dl(), order(), &unused, true ) );
            ASSERT_EQUALS( 1, unused );
            ASSERT_EQUALS( 3, nsdetails( ns.c_str() )->stats.nrecords );
            ArtificialTree::checkStructure( "{a:null,_:{c:null,_:{d:null}}}", id() );
            ASSERT( bt()->keyNode( 0 ).recordLoc.getOfs() & 1 ); // check 'unused' key
        }
    };

    class DelInternalSplitPromoteLeft : public Base {
    public:
        void run() {
            string ns = id().indexNamespace();
            ArtificialTree::setTree( "{$10:null,$20:null,$30$10:{$25:{$23:null},_:{$27:null}},$40:null,$50:null,$60:null,$70:null,$80:null,$90:null,$100:null}", id() );
            long long unused = 0;
            ASSERT_EQUALS( 13, bt()->fullValidate( dl(), order(), &unused, true ) );
            ASSERT_EQUALS( 0, unused );
            ASSERT_EQUALS( 4, nsdetails( ns.c_str() )->stats.nrecords );
            BSONObj k = BSON( "" << bigNumString( 0x30, 0x10 ) );
//            dump();
            ASSERT( unindex( k ) );
//            dump();
            ASSERT_EQUALS( 12, bt()->fullValidate( dl(), order(), &unused, true ) );
            ASSERT_EQUALS( 0, unused );
            ASSERT_EQUALS( 4, nsdetails( ns.c_str() )->stats.nrecords );
            ArtificialTree::checkStructure( "{$60:{$10:null,$20:null,$27:{$23:null,$25:null},$40:null,$50:null},_:{$70:null,$80:null,$90:null,$100:null}}", id() );
        }
    };

    class DelInternalSplitPromoteRight : public Base {
    public:
        void run() {
            string ns = id().indexNamespace();
            ArtificialTree::setTree( "{$10:null,$20:null,$30:null,$40:null,$50:null,$60:null,$70:null,$80:null,$90:null,$100$10:{$95:{$93:null},_:{$97:null}}}", id() );
            long long unused = 0;
            ASSERT_EQUALS( 13, bt()->fullValidate( dl(), order(), &unused, true ) );
            ASSERT_EQUALS( 0, unused );
            ASSERT_EQUALS( 4, nsdetails( ns.c_str() )->stats.nrecords );
            BSONObj k = BSON( "" << bigNumString( 0x100, 0x10 ) );
//            dump();
            ASSERT( unindex( k ) );
//            dump();
            ASSERT_EQUALS( 12, bt()->fullValidate( dl(), order(), &unused, true ) );
            ASSERT_EQUALS( 0, unused );
            ASSERT_EQUALS( 4, nsdetails( ns.c_str() )->stats.nrecords );
            ArtificialTree::checkStructure( "{$80:{$10:null,$20:null,$30:null,$40:null,$50:null,$60:null,$70:null},_:{$90:null,$97:{$93:null,$95:null}}}", id() );
        }
    };

    class SignedZeroDuplication : public Base {
    public:
        void run() {
            ASSERT_EQUALS( 0.0, -0.0 );
            DBDirectClient c;
            c.ensureIndex( ns(), BSON( "b" << 1 ), true );
            c.insert( ns(), BSON( "b" << 0.0 ) );
            c.insert( ns(), BSON( "b" << 1.0 ) );
            c.update( ns(), BSON( "b" << 1.0 ), BSON( "b" << -0.0 ) );
            ASSERT_EQUALS( 1U, c.count( ns(), BSON( "b" << 0.0 ) ) );
        }
    };

    class All : public Suite {
    public:
        All() : Suite( testName ) {
        }

        void setupTests() {
            add< Create >();
            add< SimpleInsertDelete >();
            add< SplitRightHeavyBucket >();
            add< SplitLeftHeavyBucket >();
            add< MissingLocate >();
            add< MissingLocateMultiBucket >();
            add< SERVER983 >();
            add< DontReuseUnused >();
            add< PackUnused >();
            add< DontDropReferenceKey >();
            add< MergeBucketsLeft >();
            add< MergeBucketsRight >();
//            add< MergeBucketsHead >();
            add< MergeBucketsDontReplaceHead >();
            add< MergeBucketsDelInternal >();
            add< MergeBucketsRightNull >();
            add< DontMergeSingleBucket >();
            add< ParentMergeNonRightToLeft >();
            add< ParentMergeNonRightToRight >();
            add< CantMergeRightNoMerge >();
            add< CantMergeLeftNoMerge >();
            add< MergeOption >();
            add< ForceMergeLeft >();
            add< ForceMergeRight >();
            add< RecursiveMerge >();
            add< RecursiveMergeRightBucket >();
            add< RecursiveMergeDoubleRightBucket >();
            add< MergeSizeJustRightRight >();
            add< MergeSizeJustRightLeft >();
            add< MergeSizeRight >();
            add< MergeSizeLeft >();
            add< NoMergeBelowMarkRight >();
            add< NoMergeBelowMarkLeft >();
            add< MergeSizeRightTooBig >();
            add< MergeSizeLeftTooBig >();
            add< BalanceOneLeftToRight >();
            add< BalanceOneRightToLeft >();
            add< BalanceThreeLeftToRight >();
            add< BalanceThreeRightToLeft >();
            add< BalanceSingleParentKey >();
            add< PackEmpty >();
            add< PackedDataSizeEmpty >();
            add< BalanceSingleParentKeyPackParent >();
            add< BalanceSplitParent >();
            add< EvenRebalanceLeft >();
            add< EvenRebalanceLeftCusp >();
            add< EvenRebalanceRight >();
            add< EvenRebalanceRightCusp >();
            add< EvenRebalanceCenter >();
            add< OddRebalanceLeft >();
            add< OddRebalanceRight >();
            add< OddRebalanceCenter >();
            add< RebalanceEmptyRight >();
            add< RebalanceEmptyLeft >();
            add< NoMoveAtLowWaterMarkRight >();
            add< MoveBelowLowWaterMarkRight >();
            add< NoMoveAtLowWaterMarkLeft >();
            add< MoveBelowLowWaterMarkLeft >();
            add< PreferBalanceLeft >();
            add< PreferBalanceRight >();
            add< RecursiveMergeThenBalance >();
            add< MergeRightEmpty >();
            add< MergeMinRightEmpty >();
            add< MergeLeftEmpty >();
            add< MergeMinLeftEmpty >();
            add< BalanceRightEmpty >();
            add< BalanceLeftEmpty >();
            add< DelEmptyNoNeighbors >();
            add< DelEmptyEmptyNeighbors >();
            add< DelInternal >();
            add< DelInternalReplaceWithUnused >();
            add< DelInternalReplaceRight >();
            add< DelInternalPromoteKey >();
            add< DelInternalPromoteRightKey >();
            add< DelInternalReplacementPrevNonNull >();
            add< DelInternalReplacementNextNonNull >();
            add< DelInternalSplitPromoteLeft >();
            add< DelInternalSplitPromoteRight >();
            add< SignedZeroDuplication >();
        }
    } myall;
