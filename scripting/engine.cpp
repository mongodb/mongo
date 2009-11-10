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

#include "stdafx.h"
#include "engine.h"
#include "../util/file.h"
#include "../client/dbclient.h"

namespace mongo {

    long long Scope::_lastVersion = 1;
    
    int Scope::_numScopes = 0;

    Scope::Scope() : _localDBName("") , _loadedVersion(0){
        _numScopes++;
    }

    Scope::~Scope(){
        _numScopes--;
    }

    ScriptEngine::ScriptEngine(){
    }

    ScriptEngine::~ScriptEngine(){
    }

    void Scope::append( BSONObjBuilder & builder , const char * fieldName , const char * scopeName ){
        int t = type( scopeName );
        
        switch ( t ){
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
            builder.appendDate( fieldName , (unsigned long long) getNumber( scopeName ) );
            break;
        default:
            stringstream temp;
            temp << "can't append type from:";
            temp << t;
            uassert( temp.str() , 0 );
        }
        
    }

    int Scope::invoke( const char* code , const BSONObj& args, int timeoutMs ){
        ScriptingFunction func = createFunction( code );
        uassert( "compile failed" , func );
        return invoke( func , args, timeoutMs );
    }
    
    bool Scope::execFile( const string& filename , bool printResult , bool reportError , bool assertOnError, int timeoutMs ){
        
        path p( filename );

        if ( ! exists( p ) ){
            cout << "file [" << filename << "] doesn't exist" << endl;
            if ( assertOnError )
                assert( 0 );
            return false;
        }

        if ( is_directory( p ) ){
            cout << "can't read directory [" << filename << "]" << endl;
            if ( assertOnError )
                assert( 0 );
            return false;
        }
        
        File f;
        f.open( filename.c_str() );

        fileofs L = f.len();
        assert( L <= 0x7ffffffe );
        char * data = (char*)malloc( (size_t) L+1 );
        data[L] = 0;
        f.read( 0 , data , (size_t) L );
        
        return exec( data , filename , printResult , reportError , assertOnError, timeoutMs );
    }

    void Scope::storedFuncMod(){
        _lastVersion++;
    }

    void Scope::loadStored( bool ignoreNotConnected ){
        if ( _localDBName.size() == 0 ){
            if ( ignoreNotConnected )
                return;
            uassert( "need to have locallyConnected already" , _localDBName.size() );
        }
        if ( _loadedVersion == _lastVersion )
            return;
        
        _loadedVersion = _lastVersion;

        static DBClientBase * db = createDirectClient();
        
        auto_ptr<DBClientCursor> c = db->query( _localDBName + ".system.js" , Query() );
        while ( c->more() ){
            BSONObj o = c->next();

            BSONElement n = o["_id"];
            BSONElement v = o["value"];
            
            uassert( "name has to be a string" , n.type() == String );
            uassert( "value has to be set" , v.type() != EOO );
            
            setElement( n.valuestr() , v );
        }
    }

    ScriptingFunction Scope::createFunction( const char * code ){
        if ( code[0] == '/' && code [1] == '*' ){
            code += 2;
            while ( code[0] && code[1] ){
                if ( code[0] == '*' && code[1] == '/' ){
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
    
    typedef map< string , list<Scope*> > PoolToScopes;

    class ScopeCache {
    public:

        ScopeCache(){
            _magic = 17;
        }
        
        ~ScopeCache(){
            assert( _magic == 17 );
            _magic = 1;

            if ( inShutdown() )
                return;
            
            clear();
        }

        void done( const string& pool , Scope * s ){
            boostlock lk( _mutex );
            list<Scope*> & l = _pools[pool];
            if ( l.size() > 10 ){
                delete s;
            }
            else {
                l.push_back( s );
                s->reset();
            }
        }
        
        Scope * get( const string& pool ){
            boostlock lk( _mutex );
            list<Scope*> & l = _pools[pool];
            if ( l.size() == 0 )
                return 0;
            
            Scope * s = l.back();
            l.pop_back();
            s->reset();
            return s;
        }
        
        void clear(){
            set<Scope*> seen;
            
            for ( PoolToScopes::iterator i=_pools.begin() ; i != _pools.end(); i++ ){
                for ( list<Scope*>::iterator j=i->second.begin(); j != i->second.end(); j++ ){
                    Scope * s = *j;
                    assert( ! seen.count( s ) );
                    delete s;
                    seen.insert( s );
                }
            }
            
            _pools.clear();
        }

    private:
        PoolToScopes _pools;
        boost::mutex _mutex;
        int _magic;
    };

    thread_specific_ptr<ScopeCache> scopeCache;

    class PooledScope : public Scope {
    public:
        PooledScope( const string pool , Scope * real ) : _pool( pool ) , _real( real ){
            _real->loadStored( true );
        };
        virtual ~PooledScope(){
            ScopeCache * sc = scopeCache.get();
            if ( sc ){
                sc->done( _pool , _real );
                _real = 0;
            }
            else {
                log() << "warning: scopeCache is empty!" << endl;
                delete _real;
                _real = 0;
            }
        }
        
        void reset(){
            _real->reset();
        }
        void init( BSONObj * data ){
            _real->init( data );
        }
        
        void localConnect( const char * dbName ){
            _real->localConnect( dbName );
        }
        void externalSetup(){
            _real->externalSetup();
        }
        
        double getNumber( const char *field ){
            return _real->getNumber( field );
        }
        string getString( const char *field ){
            return _real->getString( field );
        }
        bool getBoolean( const char *field ){
            return _real->getBoolean( field );
        }
        BSONObj getObject( const char *field ){
            return _real->getObject( field );
        }

        int type( const char *field ){
            return _real->type( field );
        }

        void setElement( const char *field , const BSONElement& val ){
            _real->setElement( field , val );
        }
        void setNumber( const char *field , double val ){
            _real->setNumber( field , val );
        }
        void setString( const char *field , const char * val ){
            _real->setString( field , val );
        }
        void setObject( const char *field , const BSONObj& obj , bool readOnly=true ){
            _real->setObject( field , obj , readOnly );
        }
        void setBoolean( const char *field , bool val ){
            _real->setBoolean( field , val );
        }
        void setThis( const BSONObj * obj ){
            _real->setThis( obj );
        }
        
        ScriptingFunction createFunction( const char * code ){
            return _real->createFunction( code );
        }

        ScriptingFunction _createFunction( const char * code ){
            return _real->createFunction( code );
        }

        /**
         * @return 0 on success
         */
        int invoke( ScriptingFunction func , const BSONObj& args, int timeoutMs , bool ignoreReturn ){
            return _real->invoke( func , args , timeoutMs , ignoreReturn );
        }

        string getError(){
            return _real->getError();
        }
        
        bool exec( const string& code , const string& name , bool printResult , bool reportError , bool assertOnError, int timeoutMs = 0 ){
            return _real->exec( code , name , printResult , reportError , assertOnError , timeoutMs );
        }
        bool execFile( const string& filename , bool printResult , bool reportError , bool assertOnError, int timeoutMs = 0 ){
            return _real->execFile( filename , printResult , reportError , assertOnError , timeoutMs );
        }
        
        void injectNative( const char *field, NativeFunction func ){
            _real->injectNative( field , func );
        }
        
        void gc(){
            _real->gc();
        }

    private:
        string _pool;
        Scope * _real;
    };

    auto_ptr<Scope> ScriptEngine::getPooledScope( const string& pool ){
        if ( ! scopeCache.get() ){
            scopeCache.reset( new ScopeCache() );
        }

        Scope * s = scopeCache->get( pool );
        if ( ! s ){
            s = createScope();
        }
        
        auto_ptr<Scope> p;
        p.reset( new PooledScope( pool , s ) );
        return p;
    }
    
    void ScriptEngine::threadDone(){
        ScopeCache * sc = scopeCache.get();
        if ( sc ){
            sc->clear();
        }
    }
    
    ScriptEngine * globalScriptEngine;
}
