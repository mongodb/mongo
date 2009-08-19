// engine.cpp

#include "stdafx.h"
#include "engine.h"
#include "../util/file.h"

namespace mongo {
    
    Scope::Scope(){
    }

    Scope::~Scope(){
    }

    ScriptEngine::ScriptEngine(){
    }

    ScriptEngine::~ScriptEngine(){
    }

    int Scope::invoke( const char* code , const BSONObj& args, int timeoutMs ){
        ScriptingFunction func = createFunction( code );
        uassert( "compile failed" , func );
        return invoke( func , args, timeoutMs );
    }

    bool Scope::execFile( const string& filename , bool printResult , bool reportError , bool assertOnError, int timeoutMs ){
        
        path p( filename );
        if ( is_directory( p ) ){
            cerr << "can't read directory [" << filename << "]" << endl;
            if ( assertOnError )
                assert( 0 );
            return false;
        }
        
        if ( ! exists( p ) ){
            cerr << "file [" << filename << "] doesn't exist" << endl;
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
        mutex _mutex;
        int _magic;
    };

    thread_specific_ptr<ScopeCache> scopeCache;

    class PooledScope : public Scope {
    public:
        PooledScope( const string pool , Scope * real ) : _pool( pool ) , _real( real ){};
        virtual ~PooledScope(){
            ScopeCache * sc = scopeCache.get();
            assert( sc );
            sc->done( _pool , _real );
            _real = 0;
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
