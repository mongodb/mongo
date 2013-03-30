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

#include "mongo/scripting/engine_spidermonkey.h"

#include <list>
#include <set>
#include <string>
#include <third_party/js-1.7/jsapi.h>
#include <third_party/js-1.7/jsdate.h>

#include "mongo/client/dbclientinterface.h"
#include "mongo/db/jsobj.h"

#define smuassert( cx , msg , val ) \
    if ( ! ( val ) ){ \
        JS_ReportError( cx , msg ); \
        return JS_FALSE; \
    }

#define CHECKNEWOBJECT(xx,ctx,w)                                   \
    if ( ! xx ){                                                   \
        massert(13072,(string)"JS_NewObject failed: " + w ,xx);    \
    }

#define CHECKJSALLOC( newthing )                \
    massert( 13615 , "JS allocation failed, either memory leak or using too much memory" , newthing )

#define smlock boost::recursive_mutex::scoped_lock ___lk( smmutex );

#define GETHOLDER(x,o) ((BSONHolder*)JS_GetPrivate( x , o ))

namespace mongo {
namespace spidermonkey {

    using std::string;

    typedef std::map<uint32_t, NativeFunction> FunctionMap;
    typedef std::map<uint32_t, void*> ArgumentMap;

    string trim( string s );

    class BSONFieldIterator;

    class BSONHolder {
    public:

        BSONHolder( BSONObj obj ) {
            _obj = obj.getOwned();
            _inResolve = false;
            _modified = false;
            _magic = 17;
        }

        ~BSONHolder() {
            _magic = 18;
        }

        void check() {
            uassert( 10212 ,  "holder magic value is wrong" , _magic == 17 && _obj.isValid() );
        }

        BSONFieldIterator * it();

        BSONObj _obj;
        bool _inResolve;
        char _magic;
        std::list<string> _extra;
        std::set<string> _removed;
        bool _modified;
    };

    class TraverseStack {
    public:
        TraverseStack() {
            _o = 0;
            _parent = 0;
        }

        TraverseStack( JSObject * o , const TraverseStack * parent ) {
            _o = o;
            _parent = parent;
        }

        TraverseStack dive( JSObject * o ) const {
            if ( o ) {
                uassert( 13076 , (string)"recursive toObject" , ! has( o ) );
            }
            return TraverseStack( o , this );
        }

        int depth() const {
            int d = 0;
            const TraverseStack * s = _parent;
            while ( s ) {
                s = s->_parent;
                d++;
            }
            return d;
        }

        bool isTop() const {
            return _parent == 0;
        }

        bool has( JSObject * o ) const {
            if ( ! o )
                return false;
            const TraverseStack * s = this;
            while ( s ) {
                if ( s->_o == o )
                    return true;
                s = s->_parent;
            }
            return false;
        }

        JSObject * _o;
        const TraverseStack * _parent;
    };

    class Convertor : boost::noncopyable {
    public:
        Convertor( JSContext * cx );

        string toString( JSString* jsString );

        string toString( jsval v );

        // NOTE No validation of passed in object
        long long toNumberLongUnsafe( JSObject *o );
        int toNumberInt( JSObject *o );
        double toNumber( jsval v );
        bool toBoolean( jsval v );
        OID toOID( jsval v );
        BSONObj toObject( JSObject * o , const TraverseStack& stack=TraverseStack() );
        BSONObj toObject( jsval v );
        string getFunctionCode( JSFunction * func );
        string getFunctionCode( jsval v );
        void appendRegex( BSONObjBuilder& b , const string& name , string s );
        void append( BSONObjBuilder& b,
                     const std::string& name,
                     jsval val,
                     BSONType oldType = EOO,
                     const TraverseStack& stack=TraverseStack() );

        // ---------- to spider monkey ---------

        bool hasFunctionIdentifier( const string& code );
        bool isSimpleStatement( const string& code );
        JSFunction * compileFunction( const char * code, JSObject * assoc = 0 );
        JSFunction * _compileFunction( const char * raw , JSObject * assoc , const char *& gcName );
        jsval toval( double d );
        jsval toval( const char * c );
        JSObject * toJSObject( const BSONObj * obj , bool readOnly=false );
        jsval toval( const BSONObj* obj , bool readOnly=false );
        void makeLongObj( long long n, JSObject * o );
        jsval toval( long long n );
        void makeIntObj( int n, JSObject * o );
        jsval toval( int n );
        jsval toval( const BSONElement& e );

        // ------- object helpers ------

        JSObject * getJSObject( JSObject * o , const char * name );
        JSObject * getGlobalObject( const char * name );
        JSObject * getGlobalPrototype( const char * name );
        bool hasProperty( JSObject * o , const char * name );
        jsval getProperty( JSObject * o , const char * field );
        void setProperty( JSObject * o , const char * field , jsval v );
        string typeString( jsval v );
        bool getBoolean( JSObject * o , const char * field );
        double getNumber( JSObject * o , const char * field );
        string getString( JSObject * o , const char * field );
        JSClass * getClass( JSObject * o , const char * field );

        JSContext * _context;
    };

    class SMScope : public Scope {
    public:
        SMScope();
        ~SMScope();

        void reset();

        void init( const BSONObj * data );

        bool hasOutOfMemoryException();

        void externalSetup();

        /** check if there is a pending killOp request */
        bool isKillPending() const;

        void localConnect( const char * dbName );

        // ----- getters ------
        double getNumber( const char *field );
        string getString( const char *field );
        bool getBoolean( const char *field );
        BSONObj getObject( const char *field );
        JSObject * getJSObject( const char * field );
        int type( const char *field );

        // ----- setters ------

        void setElement( const char *field , const BSONElement& val );
        void setNumber( const char *field , double val );
        void setString( const char *field , const StringData& val );
        void setObject( const char *field , const BSONObj& obj , bool readOnly );
        void setBoolean( const char *field , bool val );
        void setThis( const BSONObj * obj );
        void setFunction( const char *field , const char * code );
        void rename( const char * from , const char * to );

        // ---- functions -----

        ScriptingFunction _createFunction(const char* code, ScriptingFunction functionNumber);

        struct TimeoutSpec {
            boost::posix_time::ptime start;
            boost::posix_time::time_duration timeout;
            int count;
        };

        // should not generate exceptions, as those can be caught in
        // javascript code; returning false without an exception exits
        // immediately
        static JSBool _interrupt( JSContext *cx );

        static JSBool interrupt( JSContext *cx, JSScript *script ) {
            return _interrupt( cx );
        }

        void installInterrupt( int timeoutMs );

        void uninstallInterrupt( int timeoutMs );

        void precall();

        bool isReportingErrors() const { return _reportError; }

        bool exec( const StringData& code,
                   const string& name = "(anon)",
                   bool printResult = false,
                   bool reportError = true,
                   bool assertOnError = true,
                   int timeoutMs = 0 );

        int invoke( JSFunction* func,
                    const BSONObj* args,
                    const BSONObj* recv,
                    int timeoutMs,
                    bool ignoreReturn,
                    bool readOnlyArgs,
                    bool readOnlyRecv );

        int invoke( ScriptingFunction funcAddr,
                    const BSONObj* args,
                    const BSONObj* recv,
                    int timeoutMs = 0,
                    bool ignoreReturn = 0,
                    bool readOnlyArgs = false,
                    bool readOnlyRecv = false ) {
            return invoke( reinterpret_cast<JSFunction*>( funcAddr ),
                           args,
                           recv,
                           timeoutMs,
                           ignoreReturn,
                           readOnlyArgs,
                           readOnlyRecv );
        }

        void gotError( const std::string& s ) {
            _error = s;
        }

        string getError() {
            return _error;
        }

        void injectNative( const char *field, NativeFunction func, void* data );

        virtual void gc();

        JSContext *SavedContext() const { return _context; }

        // map from internal function id to function pointer
        FunctionMap _functionMap;
        // map from internal function argument id to function pointer
        ArgumentMap _argumentMap;

    private:
        void _postCreateHacks();

        JSContext * _context;
        Convertor * _convertor;

        JSObject * _global;
        JSObject * _this;

        string _error;
        bool _reportError;

        bool _externalSetup;
        bool _localConnect;

        set<string> _initFieldNames;
    };

    /**
     * Get the connection object associated with "cx" and "obj".
     */
    DBClientWithCommands *getConnection(JSContext *cx, JSObject *obj);

    /**
     * Register a function that should be part of the Mongo object in the javascript shell.
     *
     * This function may only be called from  MONGO_INITIALIZERs with SmMongoFunctionsRegistry as
     * a prerequisite, and SmMongoFunctionRegistrationDone as a dependent.
     */
    void registerMongoFunction(const JSFunctionSpec& functionSpec);

}  // namespace spidermonkey
}  // namespace mongo
