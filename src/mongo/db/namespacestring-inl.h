// namespacestring-inl.h


/**
*    Copyright (C) 2013 10gen Inc.
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

namespace mongo {

    inline StringData NamespaceString::db() const {
        return _dotIndex == string::npos ?
            StringData() :
            StringData( _ns.c_str(), _dotIndex );
    }

    inline StringData NamespaceString::coll() const {
        return _dotIndex == string::npos ?
            StringData() :
            StringData( _ns.c_str() + _dotIndex + 1, _ns.size() - 1 - _dotIndex );
    }

    inline bool NamespaceString::normal(const StringData& ns) {
        if ( ns.find( '$' ) == string::npos )
            return true;
        return oplog(ns);
    }

    inline bool NamespaceString::oplog(const StringData& ns) {
        return ns == "local.oplog.rs" || ns == "local.oplog.$main";
    }

    inline bool NamespaceString::special(const StringData& ns) {
        return !normal(ns) || ns.find( ".system." ) != string::npos;
    }

    inline bool NamespaceString::validDBName( const StringData& dbin ) {
        // XXX
        string db = dbin.toString();

        if ( db.size() == 0 || db.size() > 64 )
            return false;
#ifdef _WIN32
        // We prohibit all FAT32-disallowed characters on Windows
        size_t good = strcspn( db.c_str() , "/\\. \"*<>:|?" );
#else
        // For non-Windows platforms we are much more lenient
        size_t good = strcspn( db.c_str() , "/\\. \"" );
#endif
        return good == db.size();
    }

    inline bool NamespaceString::validCollectionName(const StringData& ns){
        size_t idx = ns.find( '.' );
        if ( idx == string::npos )
            return false;

        if ( idx + 1 >= ns.size() )
            return false;

        return normal( ns );
    }

    inline NamespaceString::NamespaceString( const StringData& nsIn ) {
        _ns = nsIn.toString(); // copy to our buffer
        _dotIndex = _ns.find( '.' );
    }

    inline int nsDBHash( const string& ns ) {
        int hash = 7;
        for ( size_t i = 0; i < ns.size(); i++ ) {
            if ( ns[i] == '.' )
                break;
            hash += 11 * ( ns[i] );
            hash *= 3;
        }
        return hash;
    }

    inline bool nsDBEquals( const string& a, const string& b ) {
        for ( size_t i = 0; i < a.size(); i++ ) {

            if ( a[i] == '.' ) {
                // b has to either be done or a '.'

                if ( b.size() == i )
                    return true;

                if ( b[i] == '.' )
                    return true;

                return false;
            }

            // a is another character
            if ( b.size() == i )
                return false;

            if ( b[i] != a[i] )
                    return false;
        }

        // a is done
        // make sure b is done
        if ( b.size() == a.size() ||
             b[a.size()] == '.' )
            return true;

        return false;
    }

    /* future : this doesn't need to be an inline. */
    inline string NamespaceString::getSisterNS( const StringData& local ) const {
        verify( local.size() && local[0] != '.' );
        return db().toString() + "." + local.toString();
    }

    inline string NamespaceString::getSystemIndexesCollection() const {
        return db().toString() + ".system.indexes";
    }

}
