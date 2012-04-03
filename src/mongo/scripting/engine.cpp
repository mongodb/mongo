// engine.cpp

/*    Copyright 2009 10gen Inc.
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

#include "pch.h"
#include "engine.h"
#include "../util/file.h"
#include "mongo/client/dbclientinterface.h"
#include "mongo/client/dbclientcursor.h"
#include <boost/filesystem/operations.hpp>

namespace mongo {

    long long Scope::_lastVersion = 1;

    int Scope::_numScopes = 0;

    Scope::Scope() : _localDBName("") , _loadedVersion(0), _numTimeUsed(0) {
        _numScopes++;
    }

    Scope::~Scope() {
        _numScopes--;
    }

    ScriptEngine::ScriptEngine() : _scopeInitCallback() {
    }

    ScriptEngine::~ScriptEngine() {
    }

    void Scope::append( BSONObjBuilder & builder , const char * fieldName , const char * scopeName ) {
        int t = type( scopeName );

        switch ( t ) {
        case Object:
            builder.append( fieldName , getObject( scopeName ) );
            break;
        case Array:
            builder.appendArray( fieldName , getObject( scopeName ) );
            break;
        case NumberDouble:
            builder.append( fieldName , getNumber( scopeName ) );
            break;
        case NumberInt:
            builder.append( fieldName , getNumberInt( scopeName ) );
            break;
        case NumberLong:
            builder.append( fieldName , getNumberLongLong( scopeName ) );
            break;
        case String:
            builder.append( fieldName , getString( scopeName ).c_str() );
            break;
        case Bool:
            builder.appendBool( fieldName , getBoolean( scopeName ) );
            break;
        case jstNULL:
        case Undefined:
            builder.appendNull( fieldName );
            break;
        case Date:
            // TODO: make signed
            builder.appendDate( fieldName , Date_t((unsigned long long)getNumber( scopeName )) );
            break;
        case Code:
            builder.appendCode( fieldName , getString( scopeName ) );
            break;
        default:
            stringstream temp;
            temp << "can't append type from:";
            temp << t;
            uassert( 10206 ,  temp.str() , 0 );
        }

    }

    int Scope::invoke( const char* code , const BSONObj* args, const BSONObj* recv, int timeoutMs ) {
        ScriptingFunction func = createFunction( code );
        uassert( 10207 ,  "compile failed" , func );
        return invoke( func , args, recv, timeoutMs );
    }

    bool Scope::execFile( const string& filename , bool printResult , bool reportError , bool assertOnError, int timeoutMs ) {

        boost::filesystem::path p( filename );

        if ( ! exists( p ) ) {
            log() << "file [" << filename << "] doesn't exist" << endl;
            if ( assertOnError )
                verify( 0 );
            return false;
        }

        // iterate directories and recurse using all *.js files in the directory
        if ( boost::filesystem::is_directory( p ) ) {
            boost::filesystem::directory_iterator end;
            bool empty = true;
            for (boost::filesystem::directory_iterator it (p); it != end; it++) {
                empty = false;
                boost::filesystem::path sub(*it);
                if (!endsWith(sub.string().c_str(), ".js"))
                    continue;
                if (!execFile(sub.string().c_str(), printResult, reportError, assertOnError, timeoutMs))
                    return false;
            }

            if (empty) {
                log() << "directory [" << filename << "] doesn't have any *.js files" << endl;
                if ( assertOnError )
                    verify( 0 );
                return false;
            }

            return true;
        }

        File f;
        f.open( filename.c_str() , true );

        unsigned L;
        {
            fileofs fo = f.len();
            verify( fo <= 0x7ffffffe );
            L = (unsigned) fo;
        }
        boost::scoped_array<char> data (new char[L+1]);
        data[L] = 0;
        f.read( 0 , data.get() , L );

        int offset = 0;
        if (data[0] == '#' && data[1] == '!') {
            const char* newline = strchr(data.get(), '\n');
            if (! newline)
                return true; // file of just shebang treated same as empty file
            offset = newline - data.get();
        }

        StringData code (data.get() + offset, L - offset);

        return exec( code , filename , printResult , reportError , assertOnError, timeoutMs );
    }

    void Scope::storedFuncMod() {
        _lastVersion++;
    }

    void Scope::validateObjectIdString( const string &str ) {
        massert( 10448 , "invalid object id: length", str.size() == 24 );

        for ( string::size_type i=0; i<str.size(); i++ ) {
            char c = str[i];
            if ( ( c >= '0' && c <= '9' ) ||
                    ( c >= 'a' && c <= 'f' ) ||
                    ( c >= 'A' && c <= 'F' ) ) {
                continue;
            }
            massert( 10430 ,  "invalid object id: not hex", false );
        }
    }

    void Scope::loadStored( bool ignoreNotConnected ) {
        if ( _localDBName.size() == 0 ) {
            if ( ignoreNotConnected )
                return;
            uassert( 10208 ,  "need to have locallyConnected already" , _localDBName.size() );
        }
        if ( _loadedVersion == _lastVersion )
            return;

        _loadedVersion = _lastVersion;

        string coll = _localDBName + ".system.js";

        static DBClientBase * db = createDirectClient();
        auto_ptr<DBClientCursor> c = db->query( coll , Query(), 0, 0, NULL, QueryOption_SlaveOk, 0 );
        verify( c.get() );

        set<string> thisTime;

        while ( c->more() ) {
            BSONObj o = c->nextSafe();

            BSONElement n = o["_id"];
            BSONElement v = o["value"];

            uassert( 10209 ,  str::stream() << "name has to be a string: " << n  , n.type() == String );
            uassert( 10210 ,  "value has to be set" , v.type() != EOO );

            setElement( n.valuestr() , v );

            thisTime.insert( n.valuestr() );
            _storedNames.insert( n.valuestr() );

        }

        // --- remove things from scope that were removed

        list<string> toremove;

        for ( set<string>::iterator i=_storedNames.begin(); i!=_storedNames.end(); i++ ) {
            string n = *i;
            if ( thisTime.count( n ) == 0 )
                toremove.push_back( n );
        }

        for ( list<string>::iterator i=toremove.begin(); i!=toremove.end(); i++ ) {
            string n = *i;
            _storedNames.erase( n );
            execSetup( (string)"delete " + n , "clean up scope" );
        }

    }

    ScriptingFunction Scope::createFunction( const char * code ) {
        if ( code[0] == '/' && code [1] == '*' ) {
            code += 2;
            while ( code[0] && code[1] ) {
                if ( code[0] == '*' && code[1] == '/' ) {
                    code += 2;
                    break;
                }
                code++;
            }
        }
        map<string,ScriptingFunction>::iterator i = _cachedFunctions.find( code );
        if ( i != _cachedFunctions.end() )
            return i->second;
        ScriptingFunction f = _createFunction( code );
        _cachedFunctions[code] = f;
        return f;
    }

    namespace JSFiles {
        extern const JSFile collection;
        extern const JSFile db;
        extern const JSFile mongo;
        extern const JSFile mr;
        extern const JSFile query;
        extern const JSFile utils;
        extern const JSFile utils_sh;
    }

    void Scope::execCoreFiles() {
        // keeping same order as in SConstruct
        execSetup(JSFiles::utils);
        execSetup(JSFiles::utils_sh);
        execSetup(JSFiles::db);
        execSetup(JSFiles::mongo);
        execSetup(JSFiles::mr);
        execSetup(JSFiles::query);
        execSetup(JSFiles::collection);
    }

    typedef map< string , list<Scope*> > PoolToScopes;

    class ScopeCache {
    public:

        ScopeCache() : _mutex("ScopeCache") {
            _magic = 17;
        }

        ~ScopeCache() {
            verify( _magic == 17 );
            _magic = 1;

            if ( inShutdown() )
                return;

            clear();
        }

        void done( const string& pool , Scope * s ) {
            scoped_lock lk( _mutex );
            list<Scope*> & l = _pools[pool];
            bool oom = s->hasOutOfMemoryException();

            // do not keep too many contexts, or use them for too long
            if ( l.size() > 10 || s->getTimeUsed() > 100 || oom ) {
                delete s;
            }
            else {
                l.push_back( s );
                s->reset();
            }

            if (oom) {
                // out of mem, make some room
                log() << "Clearing all idle JS contexts due to out of memory" << endl;
                clear();
            }
        }

        Scope * get( const string& pool ) {
            scoped_lock lk( _mutex );
            list<Scope*> & l = _pools[pool];
            if ( l.size() == 0 )
                return 0;

            Scope * s = l.back();
            l.pop_back();
            s->reset();
            s->incTimeUsed();
            return s;
        }

        void clear() {
            set<Scope*> seen;

            for ( PoolToScopes::iterator i=_pools.begin() ; i != _pools.end(); i++ ) {
                for ( list<Scope*>::iterator j=i->second.begin(); j != i->second.end(); j++ ) {
                    Scope * s = *j;
                    verify( ! seen.count( s ) );
                    delete s;
                    seen.insert( s );
                }
            }

            _pools.clear();
        }

    private:
        PoolToScopes _pools;
        mongo::mutex _mutex;
        int _magic;
    };

    thread_specific_ptr<ScopeCache> scopeCache;

    class PooledScope : public Scope {
    public:
        PooledScope( const string pool , Scope * real ) : _pool( pool ) , _real( real ) {
            _real->loadStored( true );
        };
        virtual ~PooledScope() {
            ScopeCache * sc = scopeCache.get();
            if ( sc ) {
                sc->done( _pool , _real );
                _real = 0;
            }
            else {
                // this means that the Scope was killed from a different thread
                // for example a cursor got timed out that has a $where clause
                log(3) << "warning: scopeCache is empty!" << endl;
                delete _real;
                _real = 0;
            }
        }

        void reset() {
            _real->reset();
        }
        void init( const BSONObj * data ) {
            _real->init( data );
        }

        void localConnect( const char * dbName ) {
            _real->localConnect( dbName );
        }
        void externalSetup() {
            _real->externalSetup();
        }

        double getNumber( const char *field ) {
            return _real->getNumber( field );
        }
        string getString( const char *field ) {
            return _real->getString( field );
        }
        bool getBoolean( const char *field ) {
            return _real->getBoolean( field );
        }
        BSONObj getObject( const char *field ) {
            return _real->getObject( field );
        }

        int type( const char *field ) {
            return _real->type( field );
        }

        void setElement( const char *field , const BSONElement& val ) {
            _real->setElement( field , val );
        }
        void setNumber( const char *field , double val ) {
            _real->setNumber( field , val );
        }
        void setString( const char *field , const char * val ) {
            _real->setString( field , val );
        }
        void setObject( const char *field , const BSONObj& obj , bool readOnly=true ) {
            _real->setObject( field , obj , readOnly );
        }
        void setBoolean( const char *field , bool val ) {
            _real->setBoolean( field , val );
        }
//        void setThis( const BSONObj * obj ) {
//            _real->setThis( obj );
//        }

        void setFunction( const char *field , const char * code ) {
            _real->setFunction(field, code);
        }

        ScriptingFunction createFunction( const char * code ) {
            return _real->createFunction( code );
        }

        ScriptingFunction _createFunction( const char * code ) {
            return _real->createFunction( code );
        }

        void rename( const char * from , const char * to ) {
            _real->rename( from , to );
        }

        /**
         * @return 0 on success
         */
        int invoke( ScriptingFunction func , const BSONObj* args, const BSONObj* recv, int timeoutMs , bool ignoreReturn, bool readOnlyArgs, bool readOnlyRecv ) {
            return _real->invoke( func , args , recv, timeoutMs , ignoreReturn, readOnlyArgs, readOnlyRecv );
        }

        string getError() {
            return _real->getError();
        }

        bool hasOutOfMemoryException() {
            return _real->hasOutOfMemoryException();
        }

        bool exec( const StringData& code , const string& name , bool printResult , bool reportError , bool assertOnError, int timeoutMs = 0 ) {
            return _real->exec( code , name , printResult , reportError , assertOnError , timeoutMs );
        }
        bool execFile( const string& filename , bool printResult , bool reportError , bool assertOnError, int timeoutMs = 0 ) {
            return _real->execFile( filename , printResult , reportError , assertOnError , timeoutMs );
        }

        void injectNative( const char *field, NativeFunction func, void* data ) {
            _real->injectNative( field , func, data );
        }

        void gc() {
            _real->gc();
        }

        void append( BSONObjBuilder & builder , const char * fieldName , const char * scopeName ) {
            _real->append(builder, fieldName, scopeName);
        }

    private:
        string _pool;
        Scope * _real;
    };

    auto_ptr<Scope> ScriptEngine::getPooledScope( const string& pool ) {
        if ( ! scopeCache.get() ) {
            scopeCache.reset( new ScopeCache() );
        }

        Scope * s = scopeCache->get( pool );
        if ( ! s ) {
            s = newScope();
        }

        auto_ptr<Scope> p;
        p.reset( new PooledScope( pool , s ) );
        return p;
    }

    void ScriptEngine::threadDone() {
        ScopeCache * sc = scopeCache.get();
        if ( sc ) {
            sc->clear();
        }
    }

    void ( *ScriptEngine::_connectCallback )( DBClientWithCommands & ) = 0;
    const char * ( *ScriptEngine::_checkInterruptCallback )() = 0;
    unsigned ( *ScriptEngine::_getInterruptSpecCallback )() = 0;

    ScriptEngine * globalScriptEngine = 0;

    bool hasJSReturn( const string& code ) {
        size_t x = code.find( "return" );
        if ( x == string::npos )
            return false;

        return
            ( x == 0 || ! isalpha( code[x-1] ) ) &&
            ! isalpha( code[x+6] );
    }

    const char * jsSkipWhiteSpace( const char * raw ) {
        while ( raw[0] ) {
            while (isspace(*raw)) {
                raw++;
            }

            if ( raw[0] != '/' || raw[1] != '/' )
                break;

            while ( raw[0] && raw[0] != '\n' )
                raw++;
        }
        return raw;
    }
}

