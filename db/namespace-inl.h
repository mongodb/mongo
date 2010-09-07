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

}
