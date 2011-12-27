// @file databaseholder.h

#pragma once

namespace mongo { 

    /**
     * path + dbname -> Database
     */
    class DatabaseHolder {
        typedef map<string,Database*> DBs;
        typedef map<string,DBs> Paths;
    public:
        DatabaseHolder() : _size(0) { }

        bool __isLoaded( const string& ns , const string& path ) const {
            Paths::const_iterator x = _paths.find( path );
            if ( x == _paths.end() )
                return false;
            const DBs& m = x->second;

            string db = _todb( ns );

            DBs::const_iterator it = m.find(db);
            return it != m.end();
        }
        // must be write locked as otherwise isLoaded could go false->true on you 
        // in the background and you might not expect that.
        bool _isLoaded( const string& ns , const string& path ) const {
            d.dbMutex.assertWriteLocked();
            return __isLoaded(ns,path);
        }

        Database * get( const string& ns , const string& path ) const {
            d.dbMutex.assertAtLeastReadLocked();
            Paths::const_iterator x = _paths.find( path );
            if ( x == _paths.end() )
                return 0;
            const DBs& m = x->second;
            string db = _todb( ns );
            DBs::const_iterator it = m.find(db);
            if ( it != m.end() )
                return it->second;
            return 0;
        }

        void _put( const string& ns , const string& path , Database * db ) {
            d.dbMutex.assertAtLeastReadLocked();
            DBs& m = _paths[path];
            Database*& d = m[_todb(ns)];
            if( d ) {
                dlog(2) << "info dbholder put db was already set " << ns << endl;
            }
            else {
                _size++;
            }
            d = db;
        }

        Database* getOrCreate( const string& ns , const string& path , bool& justCreated );

        void erase( const string& ns , const string& path ) {
            d.dbMutex.assertWriteLocked(); // write lock req'd as a Database obj can be in use dbHolderMutex is mainly just to control the holder itself
            DBs& m = _paths[path];
            _size -= (int)m.erase( _todb( ns ) );
        }

        /** @param force - force close even if something underway - use at shutdown */
        bool closeAll( const string& path , BSONObjBuilder& result, bool force );

        // "info" as this is informational only could change on you if you are not write locked
        int sizeInfo() const { return _size; }

        void forEach(boost::function<void(Database *)> f) const {
            d.dbMutex.assertWriteLocked();
            for ( Paths::const_iterator i=_paths.begin(); i!=_paths.end(); i++ ) {
                DBs m = i->second;
                for( DBs::const_iterator j=m.begin(); j!=m.end(); j++ ) {
                    f(j->second);
                }
            }
        }

        /**
         * gets all unique db names, ignoring paths
         */
        void getAllShortNames( bool locked, set<string>& all ) const {
            d.dbMutex.assertAtLeastReadLocked();
            for ( Paths::const_iterator i=_paths.begin(); i!=_paths.end(); i++ ) {
                DBs m = i->second;
                for( DBs::const_iterator j=m.begin(); j!=m.end(); j++ ) {
                    all.insert( j->first );
                }
            }
        }

    private:
        static string _todb( const string& ns ) {
            string d = __todb( ns );
            uassert( 13280 , (string)"invalid db name: " + ns , NamespaceString::validDBName( d ) );
            return d;
        }
        static string __todb( const string& ns ) {
            size_t i = ns.find( '.' );
            if ( i == string::npos ) {
                uassert( 13074 , "db name can't be empty" , ns.size() );
                return ns;
            }
            uassert( 13075 , "db name can't be empty" , i > 0 );
            return ns.substr( 0 , i );
        }
        Paths _paths;
        int _size;
    };

    DatabaseHolder& dbHolderUnchecked();
    inline const DatabaseHolder& dbHolder() { 
        dassert( d.dbMutex.atLeastReadLocked() );
        return dbHolderUnchecked();
    }
    inline DatabaseHolder& dbHolderW() { 
        dassert( d.dbMutex.isWriteLocked() );
        return dbHolderUnchecked();
    }

}
