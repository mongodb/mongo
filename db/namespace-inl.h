namespace mongo { 

    inline Namespace& Namespace::operator=(const char *ns) {
        uassert( 10080 , "ns name too long, max size is 128", strlen(ns) < MaxNsLen);
        //memset(buf, 0, MaxNsLen); /* this is just to keep stuff clean in the files for easy dumping and reading */
        strcpy_s(buf, MaxNsLen, ns);
        return *this;
    }

    inline string Namespace::extraName(int i) const {
        char ex[] = "$extra";
        ex[5] += i;
        string s = string(buf) + ex;
        massert( 10348 , "$extra: ns name too long", s.size() < MaxNsLen);
        return s;
    }

    inline bool Namespace::isExtra() const { 
        const char *p = strstr(buf, "$extr");
        return p && p[5] && p[6] == 0; //==0 important in case an index uses name "$extra_1" for example
    }

    inline int Namespace::hash() const {
        unsigned x = 0;
        const char *p = buf;
        while ( *p ) {
            x = x * 131 + *p;
            p++;
        }
        return (x & 0x7fffffff) | 0x8000000; // must be > 0
    }

    /* future : this doesn't need to be an inline. */
    inline string Namespace::getSisterNS( const char * local ) const {
        assert( local && local[0] != '.' );
        string old(buf);
        if ( old.find( "." ) != string::npos )
            old = old.substr( 0 , old.find( "." ) );
        return old + "." + local;
    }

    inline IndexDetails& NamespaceDetails::idx(int idxNo, bool missingExpected ) {
        if( idxNo < NIndexesBase ) 
            return _indexes[idxNo];
        Extra *e = extra();
        if ( ! e ){
            if ( missingExpected )
                throw MsgAssertionException( 13283 , "Missing Extra" );
            massert(13282, "missing Extra", e);
        }
        int i = idxNo - NIndexesBase;
        if( i >= NIndexesExtra ) {
            e = e->next(this);
            if ( ! e ){
                if ( missingExpected )
                    throw MsgAssertionException( 13283 , "missing extra" );
                massert(13283, "missing Extra", e);
            }
            i -= NIndexesExtra;
        }
        return e->details[i];
    }

    inline int NamespaceDetails::idxNo(IndexDetails& idx) { 
        IndexIterator i = ii();
        while( i.more() ) {
            if( &i.next() == &idx )
                return i.pos()-1;
        }
        massert( 10349 , "E12000 idxNo fails", false);
        return -1;
    }

    inline int NamespaceDetails::findIndexByKeyPattern(const BSONObj& keyPattern) {
        IndexIterator i = ii();
        while( i.more() ) {
            if( i.next().keyPattern() == keyPattern ) 
                return i.pos()-1;
        }
        return -1;
    }

    // @return offset in indexes[]
    inline int NamespaceDetails::findIndexByName(const char *name) {
        IndexIterator i = ii();
        while( i.more() ) {
            if ( strcmp(i.next().info.obj().getStringField("name"),name) == 0 )
                return i.pos()-1;
        }
        return -1;
    }

    inline NamespaceDetails::IndexIterator::IndexIterator(NamespaceDetails *_d) { 
        d = _d;
        i = 0;
        n = d->nIndexes;
    }

}
