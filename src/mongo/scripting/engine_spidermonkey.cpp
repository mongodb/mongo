// engine_spidermonkey.cpp

/*    Copyright 2009 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
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

#include "mongo/pch.h"

#include "mongo/scripting/engine_spidermonkey.h"

#include <boost/smart_ptr/scoped_array.hpp>
#include <boost/thread/recursive_mutex.hpp>
#ifndef _WIN32
#include <boost/date_time/posix_time/posix_time.hpp>
#endif

#include <third_party/js-1.7/jsdate.h>

#include "mongo/scripting/engine_spidermonkey_internal.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

    /* used to make the logging not overly chatty in the mongo shell. */
    extern bool isShell;

namespace spidermonkey {

    string trim( string s ) {
        while ( s.size() && isspace( s[0] ) )
            s = s.substr( 1 );

        while ( s.size() && isspace( s[s.size()-1] ) )
            s = s.substr( 0 , s.size() - 1 );

        return s;
    }

    boost::thread_specific_ptr<SMScope> currentScope( dontDeleteScope );
    boost::recursive_mutex &smmutex = *( new boost::recursive_mutex );

    class BSONFieldIterator {
    public:

        BSONFieldIterator( BSONHolder * holder ) {

            set<string> added;

            BSONObjIterator it( holder->_obj );
            while ( it.more() ) {
                BSONElement e = it.next();
                if ( holder->_removed.count( e.fieldName() ) )
                    continue;
                _names.push_back( e.fieldName() );
                added.insert( e.fieldName() );
            }

            for ( list<string>::iterator i = holder->_extra.begin(); i != holder->_extra.end(); i++ ) {
                if ( ! added.count( *i ) )
                    _names.push_back( *i );
            }

            _it = _names.begin();
        }

        bool more() {
            return _it != _names.end();
        }

        string next() {
            string s = *_it;
            _it++;
            return s;
        }

    private:
        list<string> _names;
        list<string>::iterator _it;
    };

    BSONFieldIterator * BSONHolder::it() {
        return new BSONFieldIterator( this );
    }

    Convertor::Convertor( JSContext * cx ) {
            _context = cx;
    }

    string Convertor::toString( JSString* jsString ) {
            size_t srclen = JS_GetStringLength( jsString );
            if( srclen == 0 )
                return "";

            size_t len = (srclen * 6) + 1;
            boost::scoped_array<char> utf8Chars( new char[len] );
            jschar* utf16Chars = JS_GetStringChars( jsString );
            if ( !JS_EncodeCharacters( _context, utf16Chars, srclen, utf8Chars.get(), &len ) ) {
                uasserted( 16268, "error converting UTF-16 string to UTF-8" );
            }
            return string( utf8Chars.get(), len );
    }

    string Convertor::toString( jsval v ) {
            return toString( JS_ValueToString( _context , v ) );
    }

    long long Convertor::toNumberLongUnsafe( JSObject *o ) {
            boost::uint64_t val;
            if ( hasProperty( o, "top" ) ) {
                val =
                    ( (boost::uint64_t)(boost::uint32_t)getNumber( o , "top" ) << 32 ) +
                    ( boost::uint32_t)( getNumber( o , "bottom" ) );
            }
            else {
                val = (boost::uint64_t)(boost::int64_t) getNumber( o, "floatApprox" );
            }
            return val;
    }

    int Convertor::toNumberInt( JSObject *o ) {
            return (boost::uint32_t)(boost::int32_t) getNumber( o, "floatApprox" );
    }

    double Convertor::toNumber( jsval v ) {
            double d;
            uassert( 10214 ,  "not a number" , JS_ValueToNumber( _context , v , &d ) );
            return d;
    }

    bool Convertor::toBoolean( jsval v ) {
            JSBool b;
            verify( JS_ValueToBoolean( _context, v , &b ) );
            return b;
    }

    OID Convertor::toOID( jsval v ) {
            JSContext * cx = _context;
            verify( JSVAL_IS_OID( v ) );

            JSObject * o = JSVAL_TO_OBJECT( v );
            OID oid;
            oid.init( getString( o , "str" ) );
            return oid;
        }

    BSONObj Convertor::toObject( JSObject * o , const TraverseStack& stack ) {
            if ( ! o )
                return BSONObj();

            if ( JS_InstanceOf( _context , o , &bson_ro_class , 0 ) ) {
                BSONHolder * holder = GETHOLDER( _context , o );
                verify( holder );
                return holder->_obj.getOwned();
            }

            BSONObj orig;
            if ( JS_InstanceOf( _context , o , &bson_class , 0 ) ) {
                BSONHolder * holder = GETHOLDER(_context,o);
                verify( holder );
                if ( ! holder->_modified ) {
                    return holder->_obj;
                }
                orig = holder->_obj;
            }

            BSONObjBuilder b;

            if ( ! appendSpecialDBObject( this , b , "value" , OBJECT_TO_JSVAL( o ) , o ) ) {

                if ( stack.isTop() ) {
                    jsval theid = getProperty( o , "_id" );
                    if ( ! JSVAL_IS_VOID( theid ) ) {
                        append( b , "_id" , theid , EOO , stack.dive( o ) );
                    }
                }

                JSIdArray * properties = JS_Enumerate( _context , o );
                verify( properties );

                try {
                    for ( jsint i=0; i<properties->length; i++ ) {
                        jsid id = properties->vector[i];
                        jsval nameval;
                        verify( JS_IdToValue( _context, id, &nameval ) );
                        string name = toString( nameval );
                        if ( stack.isTop() && name == "_id" )
                            continue;

                        append( b,
                                name,
                                getProperty( o, name.c_str() ),
                                orig[name].type(),
                                stack.dive( o ) );
                    }
                }
                catch ( const AssertionException& ) {
                    JS_DestroyIdArray( _context , properties );
                    throw;
                }
                catch ( const std::exception& e ) {
                    log() << "unhandled exception: " << e.what() << ", throwing Fatal Assertion" << endl;
                    fassertFailed( 16269 );
                }
                JS_DestroyIdArray( _context , properties );
            }

            return b.obj();
    }

    BSONObj Convertor::toObject( jsval v ) {
            if ( JSVAL_IS_NULL( v ) || JSVAL_IS_VOID( v ) )
                return BSONObj();

            uassert( 10215, "not an object", JSVAL_IS_OBJECT( v ) );
            return toObject( JSVAL_TO_OBJECT( v ) );
    }

    string Convertor::getFunctionCode( JSFunction * func ) {
            return toString( JS_DecompileFunction( _context , func , 0 ) );
    }

    string Convertor::getFunctionCode( jsval v ) {
            uassert( 10216, "not a function", JS_TypeOfValue( _context, v ) == JSTYPE_FUNCTION );
            return getFunctionCode( JS_ValueToFunction( _context , v ) );
    }

    void Convertor::appendRegex( BSONObjBuilder& b , const string& name , string s ) {
            verify( s[0] == '/' );
            s = s.substr(1);
            string::size_type end = s.rfind( '/' );
            b.appendRegex( name , s.substr( 0 , end ) , s.substr( end + 1 ) );
    }

    void Convertor::append( BSONObjBuilder& b,
                            const std::string& name,
                            jsval val,
                            BSONType oldType,
                            const TraverseStack& stack ) {
            //cout << "name: " << name << "\t" << typeString( val ) << " oldType: " << oldType << endl;
            switch ( JS_TypeOfValue( _context , val ) ) {

            case JSTYPE_VOID: b.appendUndefined( name ); break;
            case JSTYPE_NULL: b.appendNull( name ); break;

            case JSTYPE_NUMBER: {
                double d = toNumber( val );
                if ( oldType == NumberInt && ((int)d) == d )
                    b.append( name , (int)d );
                else
                    b.append( name , d );
                break;
            }
            case JSTYPE_STRING: b.append( name , toString( val ) ); break;
            case JSTYPE_BOOLEAN: b.appendBool( name , toBoolean( val ) ); break;

            case JSTYPE_OBJECT: {
                JSObject * o = JSVAL_TO_OBJECT( val );
                if ( ! o || o == JSVAL_NULL ) {
                    b.appendNull( name );
                }
                else if ( ! appendSpecialDBObject( this , b , name , val , o ) ) {
                    BSONObj sub = toObject( o , stack );
                    if ( JS_IsArrayObject( _context , o ) ) {
                        b.appendArray( name , sub );
                    }
                    else {
                        b.append( name , sub );
                    }
                }
                break;
            }

            case JSTYPE_FUNCTION: {
                string s = toString(val);
                if ( s[0] == '/' ) {
                    appendRegex( b , name , s );
                }
                else {
                    b.appendCode( name , getFunctionCode( val ) );
                }
                break;
            }

            default: uassert( 10217 ,  (string)"can't append field.  name:" + name + " type: " + typeString( val ) , 0 );
            }
    }

    bool Convertor::hasFunctionIdentifier( const string& code ) {
            if ( code.size() < 9 || code.find( "function" ) != 0  )
                return false;

            return code[8] == ' ' || code[8] == '(';
    }

    bool Convertor::isSimpleStatement( const string& code ) {
            if ( hasJSReturn( code ) )
                return false;

            if ( code.find( ';' ) != string::npos &&
                    code.find( ';' ) != code.rfind( ';' ) )
                return false;

            if ( code.find( '\n') != string::npos )
                return false;

            if ( code.find( "for(" ) != string::npos ||
                    code.find( "for (" ) != string::npos ||
                    code.find( "while (" ) != string::npos ||
                    code.find( "while(" ) != string::npos )
                return false;

            return true;
    }

    JSFunction * Convertor::compileFunction( const char * code, JSObject * assoc ) {
            const char * gcName = "unknown";
            JSFunction * f = _compileFunction( code , assoc , gcName );
            return f;
    }

    JSFunction * Convertor::_compileFunction( 
            const char * raw , JSObject * assoc , const char *& gcName ) {

            if ( ! assoc )
                assoc = JS_GetGlobalObject( _context );

            raw = jsSkipWhiteSpace( raw );

            //cout << "RAW\n---\n" << raw << "\n---" << endl;

            static int fnum = 1;
            stringstream fname;
            fname << "__cf__" << fnum++ << "__";

            if ( ! hasFunctionIdentifier( raw ) ) {
                string s = raw;
                if ( isSimpleStatement( s ) ) {
                    s = "return " + s;
                }
                gcName = "cf anon";
                fname << "anon";
                return JS_CompileFunction( _context , assoc , fname.str().c_str() , 0 , 0 , s.c_str() , s.size() , "nofile_a" , 0 );
            }

            string code = raw;

            size_t start = code.find( '(' );
            verify( start != string::npos );

            string fbase;
            if ( start > 9 ) {
                fbase = trim( code.substr( 9 , start - 9 ) );
            }
            if ( fbase.length() == 0 ) {
                fbase = "anonymous_function";
            }
            fname << "f__" << fbase;

            code = code.substr( start + 1 );
            size_t end = code.find( ')' );
            verify( end != string::npos );

            string paramString = trim( code.substr( 0 , end ) );
            code = code.substr( end + 1 );

            vector<string> params;
            while ( paramString.size() ) {
                size_t c = paramString.find( ',' );
                if ( c == string::npos ) {
                    params.push_back( paramString );
                    break;
                }
                params.push_back( trim( paramString.substr( 0 , c ) ) );
                paramString = trim( paramString.substr( c + 1 ) );
                paramString = trim( paramString );
            }

            boost::scoped_array<const char *> paramArray (new const char*[params.size()]);
            for ( size_t i=0; i<params.size(); i++ )
                paramArray[i] = params[i].c_str();
            
            // avoid munging previously munged name (kludge; switching to v8 fixes underlying issue)
            if ( fbase.find("__cf__") != 0 && fbase.find("__f__") == string::npos ) {
                fbase = fname.str();
            }

            JSFunction * func = JS_CompileFunction( _context , assoc , fbase.c_str() , params.size() , paramArray.get() , code.c_str() , code.size() , "nofile_b" , 0 );

            if ( ! func ) {
                log() << "compile failed for: " << raw << endl;
                return 0;
            }
            gcName = "cf normal";
            return func;
    }

    jsval Convertor::toval( double d ) {
            jsval val;
            verify( JS_NewNumberValue( _context, d , &val ) );
            return val;
    }

    jsval Convertor::toval( const char * c ) {
            JSString * s = JS_NewStringCopyZ( _context , c );
            if ( s )
                return STRING_TO_JSVAL( s );

            // possibly unicode, try manual

            size_t len = strlen( c );
            size_t dstlen = len * 4;
            jschar * dst = (jschar*)malloc( dstlen );

            JSBool res = JS_DecodeBytes( _context , c , len , dst, &dstlen );
            if ( res ) {
                s = JS_NewUCStringCopyN( _context , dst , dstlen );
            }

            free( dst );

            if ( ! res ) {
                tlog() << "decode failed. probably invalid utf-8 string [" << c << "]" << endl;
                jsval v;
                if ( JS_GetPendingException( _context , &v ) )
                    tlog() << "\t why: " << toString( v ) << endl;
                uassert( 16270, "conversion from string to JavaScript value failed", false );
            }

            CHECKJSALLOC( s );
            return STRING_TO_JSVAL( s );
    }

    JSObject * Convertor::toJSObject( const BSONObj * obj , bool readOnly ) {
            static string ref = "$ref";
            if ( ref == obj->firstElementFieldName() ) {
                JSObject * o = JS_NewObject( _context , &dbref_class , NULL, NULL);
                CHECKNEWOBJECT(o,_context,"toJSObject1");
                verify( JS_SetPrivate( _context , o , (void*)(new BSONHolder( obj->getOwned() ) ) ) );
                return o;
            }
            JSObject * o = JS_NewObject( _context , readOnly ? &bson_ro_class : &bson_class , NULL, NULL);
            CHECKNEWOBJECT(o,_context,"toJSObject2");
            verify( JS_SetPrivate( _context , o , (void*)(new BSONHolder( obj->getOwned() ) ) ) );
            return o;
    }

    jsval Convertor::toval( const BSONObj* obj , bool readOnly ) {
            JSObject * o = toJSObject( obj , readOnly );
            return OBJECT_TO_JSVAL( o );
    }

    void Convertor::makeLongObj( long long n, JSObject * o ) {
            boost::uint64_t val = (boost::uint64_t)n;
            CHECKNEWOBJECT(o,_context,"NumberLong1");
            double floatApprox = (double)(boost::int64_t)val;
            setProperty( o , "floatApprox" , toval( floatApprox ) );
            if ( (boost::int64_t)val != (boost::int64_t)floatApprox ) {
                // using 2 doubles here instead of a single double because certain double
                // bit patterns represent undefined values and sm might trash them
                setProperty( o , "top" , toval( (double)(boost::uint32_t)( val >> 32 ) ) );
                setProperty( o , "bottom" , toval( (double)(boost::uint32_t)( val & 0x00000000ffffffff ) ) );
            }
    }

    jsval Convertor::toval( long long n ) {
            JSObject * o = JS_NewObject( _context , &numberlong_class , 0 , 0 );
            makeLongObj( n, o );
            return OBJECT_TO_JSVAL( o );
    }

    void Convertor::makeIntObj( int n, JSObject * o ) {
            boost::uint32_t val = (boost::uint32_t)n;
            CHECKNEWOBJECT(o,_context,"NumberInt1");
            double floatApprox = (double)(boost::int32_t)val;
            setProperty( o , "floatApprox" , toval( floatApprox ) );
    }

    jsval Convertor::toval( int n ) {
            JSObject * o = JS_NewObject( _context , &numberint_class , 0 , 0 );
            makeIntObj( n, o );
            return OBJECT_TO_JSVAL( o );
    }

    jsval Convertor::toval( const BSONElement& e ) {

            switch( e.type() ) {
            case EOO:
            case jstNULL:
            case Undefined:
                return JSVAL_NULL;
            case NumberDouble:
            case NumberInt:
                return toval( e.number() );
//            case NumberInt:
//                return toval( e.numberInt() );
            case Symbol: // TODO: should we make a special class for this
            case String:
                return toval( e.valuestr() );
            case Bool:
                return e.boolean() ? JSVAL_TRUE : JSVAL_FALSE;
            case Object: {
                BSONObj embed = e.embeddedObject().getOwned();
                return toval( &embed );
            }
            case Array: {

                BSONObj embed = e.embeddedObject().getOwned();

                if ( embed.isEmpty() ) {
                    return OBJECT_TO_JSVAL( JS_NewArrayObject( _context , 0 , 0 ) );
                }
                
                JSObject * array = JS_NewArrayObject( _context , 1 , 0 );
                CHECKJSALLOC( array );

                jsval myarray = OBJECT_TO_JSVAL( array );

                BSONObjIterator i( embed );
                while ( i.more() ){
                    const BSONElement& e = i.next();
                    jsval v = toval( e );
                    verify( JS_SetElement( _context , array , atoi(e.fieldName()) , &v ) );
                }

                return myarray;
            }
            case jstOID: {
                OID oid = e.__oid();
                JSObject * o = JS_NewObject( _context , &object_id_class , 0 , 0 );
                CHECKNEWOBJECT(o,_context,"jstOID");
                setProperty( o , "str" , toval( oid.str().c_str() ) );
                return OBJECT_TO_JSVAL( o );
            }
            case RegEx: {
                const char * flags = e.regexFlags();
                uintN flagNumber = 0;
                while ( *flags ) {
                    switch ( *flags ) {
                    case 'g': flagNumber |= JSREG_GLOB; break;
                    case 'i': flagNumber |= JSREG_FOLD; break;
                    case 'm': flagNumber |= JSREG_MULTILINE; break;
                        //case 'y': flagNumber |= JSREG_STICKY; break;

                    default:
                        log() << "warning: unknown regex flag:" << *flags << endl;
                    }
                    flags++;
                }

                JSObject * r = JS_NewRegExpObject( _context , (char*)e.regex() , strlen( e.regex() ) , flagNumber );
                verify( r );
                return OBJECT_TO_JSVAL( r );
            }
            case Code: {
                JSFunction * func = compileFunction( e.valuestr() );
                if ( func )
                    return OBJECT_TO_JSVAL( JS_GetFunctionObject( func ) );
                return JSVAL_NULL;
            }
            case CodeWScope: {
                JSFunction * func = compileFunction( e.codeWScopeCode() );
                if ( !func )
                    return JSVAL_NULL;

                BSONObj extraScope = e.codeWScopeObject();
                if ( ! extraScope.isEmpty() ) {
                    log() << "warning: CodeWScope doesn't transfer to db.eval" << endl;
                }

                return OBJECT_TO_JSVAL( JS_GetFunctionObject( func ) );
            }
            case Date:
                return OBJECT_TO_JSVAL( js_NewDateObjectMsec( _context , (jsdouble) ((long long)e.date().millis) ) );

            case MinKey:
                return OBJECT_TO_JSVAL( JS_NewObject( _context , &minkey_class , 0 , 0 ) );

            case MaxKey:
                return OBJECT_TO_JSVAL( JS_NewObject( _context , &maxkey_class , 0 , 0 ) );

            case Timestamp: {
                JSObject * o = JS_NewObject( _context , &timestamp_class , 0 , 0 );
                CHECKNEWOBJECT(o,_context,"Timestamp1");
                setProperty( o , "t" , toval( (double)(e.timestampTime() / 1000) ) );
                setProperty( o , "i" , toval( (double)(e.timestampInc()) ) );
                return OBJECT_TO_JSVAL( o );
            }
            case NumberLong: {
                return toval( e.numberLong() );
            }
            case DBRef: {
                JSObject * o = JS_NewObject( _context , &dbpointer_class , 0 , 0 );
                CHECKNEWOBJECT(o,_context,"DBRef1");
                setProperty( o , "ns" , toval( e.dbrefNS() ) );

                JSObject * oid = JS_NewObject( _context , &object_id_class , 0 , 0 );
                CHECKNEWOBJECT(oid,_context,"DBRef2");
                setProperty( oid , "str" , toval( e.dbrefOID().str().c_str() ) );

                setProperty( o , "id" , OBJECT_TO_JSVAL( oid ) );
                return OBJECT_TO_JSVAL( o );
            }
            case BinData: {
                JSObject * o = JS_NewObject( _context , &bindata_class , 0 , 0 );
                CHECKNEWOBJECT(o,_context,"Bindata_BinData1");
                int len;
                const char * data = e.binData( len );
                verify( data );
                verify( JS_SetPrivate( _context , o , new BinDataHolder( data , len ) ) );

                setProperty( o , "len" , toval( (double)len ) );
                setProperty( o , "type" , toval( (double)e.binDataType() ) );
                return OBJECT_TO_JSVAL( o );
            }
            }

            log() << "toval: unknown type: " << (int) e.type() << endl;
            uassert( 10218 ,  "not done: toval" , 0 );
            return 0;
    }

    JSObject * Convertor::getJSObject( JSObject * o , const char * name ) {
            jsval v;
            verify( JS_GetProperty( _context , o , name , &v ) );
            return JSVAL_TO_OBJECT( v );
    }

    JSObject * Convertor::getGlobalObject( const char * name ) {
            return getJSObject( JS_GetGlobalObject( _context ) , name );
    }

    JSObject * Convertor::getGlobalPrototype( const char * name ) {
            return getJSObject( getGlobalObject( name ) , "prototype" );
    }

    bool Convertor::hasProperty( JSObject * o , const char * name ) {
            JSBool res;
            verify( JS_HasProperty( _context , o , name , & res ) );
            return res;
    }

    jsval Convertor::getProperty( JSObject * o , const char * field ) {
            uassert( 10219 ,  "object passed to getPropery is null" , o );
            jsval v;
            verify( JS_GetProperty( _context , o , field , &v ) );
            return v;
    }

    void Convertor::setProperty( JSObject * o , const char * field , jsval v ) {
            verify( JS_SetProperty( _context , o , field , &v ) );
    }

    string Convertor::typeString( jsval v ) {
            JSType t = JS_TypeOfValue( _context , v );
            return JS_GetTypeName( _context , t );
        }

    bool Convertor::getBoolean( JSObject * o , const char * field ) {
            return toBoolean( getProperty( o , field ) );
    }

    double Convertor::getNumber( JSObject * o , const char * field ) {
            return toNumber( getProperty( o , field ) );
    }

    string Convertor::getString( JSObject * o , const char * field ) {
            return toString( getProperty( o , field ) );
    }

    JSClass * Convertor::getClass( JSObject * o , const char * field ) {
            jsval v;
            verify( JS_GetProperty( _context , o , field , &v ) );
            if ( ! JSVAL_IS_OBJECT( v ) )
                return 0;
            return JS_GET_CLASS( _context , JSVAL_TO_OBJECT( v ) );
    }

    void bson_finalize( JSContext * cx , JSObject * obj ) {
        try {
            BSONHolder * o = GETHOLDER( cx , obj );
            if ( o ) {
                delete o;
                verify( JS_SetPrivate( cx , obj , 0 ) );
            }
        }
        catch ( const AssertionException& e ) {
            if ( ! JS_IsExceptionPending( cx ) ) JS_ReportError( cx, e.what() );
        }
        catch ( const std::exception& e ) {
            log() << "unhandled exception: " << e.what() << ", throwing Fatal Assertion" << endl;
            fassertFailed( 16271 );
        }
    }

    JSBool bson_enumerate( JSContext *cx, JSObject *obj, JSIterateOp enum_op, jsval *statep, jsid *idp ) {
        try {
            BSONHolder * o = GETHOLDER( cx , obj );

            if ( enum_op == JSENUMERATE_INIT ) {
                if ( o ) {
                    BSONFieldIterator * it = o->it();
                    *statep = PRIVATE_TO_JSVAL( it );
                }
                else {
                    *statep = 0;
                }
                if ( idp )
                    *idp = JSVAL_ZERO;
                return JS_TRUE;
            }

            BSONFieldIterator * it = (BSONFieldIterator*)JSVAL_TO_PRIVATE( *statep );
            if ( ! it ) {
                *statep = 0;
                return JS_TRUE;
            }

            if ( enum_op == JSENUMERATE_NEXT ) {
                if ( it->more() ) {
                    string name = it->next();
                    Convertor c(cx);
                    verify( JS_ValueToId( cx , c.toval( name.c_str() ) , idp ) );
                }
                else {
                    delete it;
                    *statep = 0;
                }
                return JS_TRUE;
            }

            if ( enum_op == JSENUMERATE_DESTROY ) {
                if ( it )
                    delete it;
                return JS_TRUE;
            }

            uassert( 10220 ,  "don't know what to do with this op" , 0 );
            return JS_FALSE;
        }
        catch ( const AssertionException& e ) {
            if ( ! JS_IsExceptionPending( cx ) ) {
                JS_ReportError( cx, e.what() );
            }
            return JS_FALSE;
        }
        catch ( const std::exception& e ) {
            log() << "unhandled exception: " << e.what() << ", throwing Fatal Assertion" << endl;
            fassertFailed( 16272 );
        }
    }

    JSBool noaccess( JSContext *cx, JSObject *obj, jsval idval, jsval *vp) {
        try {
            BSONHolder * holder = GETHOLDER( cx , obj );
            if ( ! holder ) {
                // in init code still
                return JS_TRUE;
            }
            if ( holder->_inResolve )
                return JS_TRUE;
            JS_ReportError( cx , "doing write op on read only operation" );
            return JS_FALSE;
        }
        catch ( const AssertionException& e ) {
            if ( ! JS_IsExceptionPending( cx ) ) {
                JS_ReportError( cx, e.what() );
            }
            return JS_FALSE;
        }
        catch ( const std::exception& e ) {
            log() << "unhandled exception: " << e.what() << ", throwing Fatal Assertion" << endl;
            fassertFailed( 16273 );
        }
    }

    JSClass bson_ro_class = {
        "bson_ro_object",                                                   // class name
        JSCLASS_HAS_PRIVATE | JSCLASS_NEW_RESOLVE | JSCLASS_NEW_ENUMERATE,  // flags
        noaccess,                                                           // addProperty
        noaccess,                                                           // delProperty
        JS_PropertyStub,                                                    // getProperty
        noaccess,                                                           // setProperty
        (JSEnumerateOp)bson_enumerate,                                      // enumerate
        (JSResolveOp)resolveBSONField,                                      // resolve
        JS_ConvertStub,                                                     // convert
        bson_finalize,                                                      // finalize
        JSCLASS_NO_OPTIONAL_MEMBERS                                         // optional members
    };

    JSBool bson_add_prop( JSContext *cx, JSObject *obj, jsval idval, jsval *vp) {
        try {
            BSONHolder * holder = GETHOLDER( cx , obj );
            if ( ! holder ) {
                // static init
                return JS_TRUE;
            }
            if ( ! holder->_inResolve ) {
                Convertor c(cx);
                string name( c.toString( idval ) );
                if ( holder->_obj[name].eoo() ) {
                    holder->_extra.push_back( name );
                }
                holder->_modified = true;
            }
        }
        catch ( const AssertionException& e ) {
            if ( ! JS_IsExceptionPending( cx ) ) {
                JS_ReportError( cx, e.what() );
            }
            return JS_FALSE;
        }
        catch ( const std::exception& e ) {
            log() << "unhandled exception: " << e.what() << ", throwing Fatal Assertion" << endl;
            fassertFailed( 16274 );
        }
        return JS_TRUE;
    }


    JSBool mark_modified( JSContext *cx, JSObject *obj, jsval idval, jsval *vp) {
        try {
            Convertor c(cx);
            BSONHolder * holder = GETHOLDER( cx , obj );
            if ( !holder ) // needed when we're messing with DBRef.prototype
                return JS_TRUE;
            if ( holder->_inResolve )
                return JS_TRUE;
            holder->_modified = true;
            holder->_removed.erase( c.toString( idval ) );
        }
        catch ( const AssertionException& e ) {
            if ( ! JS_IsExceptionPending( cx ) ) {
                JS_ReportError( cx, e.what() );
            }
            return JS_FALSE;
        }
        catch ( const std::exception& e ) {
            log() << "unhandled exception: " << e.what() << ", throwing Fatal Assertion" << endl;
            fassertFailed( 16275 );
        }
        return JS_TRUE;
    }

    JSBool mark_modified_remove( JSContext *cx, JSObject *obj, jsval idval, jsval *vp) {
        try {
            Convertor c(cx);
            BSONHolder * holder = GETHOLDER( cx , obj );
            if ( holder->_inResolve )
                return JS_TRUE;
            holder->_modified = true;
            holder->_removed.insert( c.toString( idval ) );
        }
        catch ( const AssertionException& e ) {
            if ( ! JS_IsExceptionPending( cx ) ) {
                JS_ReportError( cx, e.what() );
            }
            return JS_FALSE;
        }
        catch ( const std::exception& e ) {
            log() << "unhandled exception: " << e.what() << ", throwing Fatal Assertion" << endl;
            fassertFailed( 16276 );
        }
        return JS_TRUE;
    }

    JSClass bson_class = {
        "bson_object",                                                      // class name
        JSCLASS_HAS_PRIVATE | JSCLASS_NEW_RESOLVE | JSCLASS_NEW_ENUMERATE,  // flags
        bson_add_prop,                                                      // addProperty
        mark_modified_remove,                                               // delProperty
        JS_PropertyStub,                                                    // getProperty
        mark_modified,                                                      // setProperty
        (JSEnumerateOp)bson_enumerate,                                      // enumerate
        (JSResolveOp)resolveBSONField,                                      // resolve
        JS_ConvertStub,                                                     // convert
        bson_finalize,                                                      // finalize
        JSCLASS_NO_OPTIONAL_MEMBERS                                         // optional members
    };

    static JSClass global_class = {
        "global",                       // class name
        JSCLASS_GLOBAL_FLAGS,           // flags
        JS_PropertyStub,                // addProperty
        JS_PropertyStub,                // delProperty
        JS_PropertyStub,                // getProperty
        JS_PropertyStub,                // setProperty
        JS_EnumerateStub,               // enumerate
        JS_ResolveStub,                 // resolve
        JS_ConvertStub,                 // convert
        JS_FinalizeStub,                // finalize
        JSCLASS_NO_OPTIONAL_MEMBERS     // optional members
    };

    // --- global helpers ---

    static void hexToBinData( JSContext* cx,
                              Convertor* c,
                              jsval* rval,
                              int subtype,
                              const string& s ) {
        JSObject * o = JS_NewObject( cx , &bindata_class , 0 , 0 );
        CHECKNEWOBJECT(o,_context,"Bindata_BinData1");
        size_t s_size = s.size();
        int len = s_size / 2;
        boost::scoped_array<char> data( new char[len] );
        char* p = data.get();
        const char *src = s.c_str();
        for( size_t i = 0; i+1 < s_size; i += 2 ) { 
            *p++ = fromHex( src + i );
        }
        verify( JS_SetPrivate( cx, o, new BinDataHolder( data.get(), len ) ) );
        c->setProperty( o, "len", c->toval( static_cast<double>(len) ) );
        c->setProperty( o, "type", c->toval( static_cast<double>(subtype) ) );
        *rval = OBJECT_TO_JSVAL( o );
    }

    static bool testHexString( JSContext* cx, const string& hexString ) {
        size_t len = hexString.length();
        for ( size_t i = 0; i < len; ++i ) {
            char ch = hexString[i];
            if ( (ch>='0' && ch<='9') || (ch>='a' && ch<='f') || (ch>='A' && ch<='F') ) {
                continue;
            }
            JS_ReportError( cx, "invalid hexstring character '%c'", ch );
            return false;
        }
        return true;
    }

    JSBool _HexData( JSContext * cx , JSObject * obj , uintN argc, jsval *argv, jsval *rval ) {
        try {
            if ( argc != 2 ) {
                JS_ReportError( cx , "HexData needs 2 arguments -- HexData(subtype,hexstring)" );
                return JS_FALSE;
            }
            Convertor c( cx );
            int subtype = static_cast<int>( c.toNumber( argv[ 0 ] ) );
            if ( subtype == 2 ) {
                JS_ReportError( cx , "BinData subtype 2 is deprecated" );
                return JS_FALSE;
            }
            else if ( subtype < 0 || subtype > 255 ) {
                JS_ReportError( cx, "subtype must be between 0 and 255" );
                return JS_FALSE;
            }
            string s( c.toString( argv[1] ) );
            if ( ! testHexString( cx, s ) ) {
                return JS_FALSE;
            }
            size_t len = s.length();
            if ( 0 != ( len % 2 ) ) {
                JS_ReportError( cx, "hexstring must be even length" );
                return JS_FALSE;
            }
            hexToBinData(cx, &c, rval, subtype, s);
        }
        catch ( const AssertionException& e ) {
            if ( ! JS_IsExceptionPending( cx ) ) {
                JS_ReportError( cx, e.what() );
            }
            return JS_FALSE;
        }
        catch ( const std::exception& e ) {
            log() << "unhandled exception: " << e.what() << ", throwing Fatal Assertion" << endl;
            fassertFailed( 16277 );
        }
        return JS_TRUE;
    }

    JSBool _UUID( JSContext * cx , JSObject * obj , uintN argc, jsval *argv, jsval *rval ) {
        try {
            if ( argc != 1 ) {
                JS_ReportError( cx , "UUID needs argument -- UUID(hexstring)" );
                return JS_FALSE;
            }
            Convertor c( cx );
            string s( c.toString( argv[0] ) );
            if ( ! testHexString( cx, s ) ) {
                return JS_FALSE;
            }
            size_t len = s.length();
            if ( len != 32 ) {
                JS_ReportError( cx, "UUID hexstring length is %d, must be 32", len );
                return JS_FALSE;
            }
            hexToBinData(cx, &c, rval, 3, s);
        }
        catch ( const AssertionException& e ) {
            if ( ! JS_IsExceptionPending( cx ) ) {
                JS_ReportError( cx, e.what() );
            }
            return JS_FALSE;
        }
        catch ( const std::exception& e ) {
            log() << "unhandled exception: " << e.what() << ", throwing Fatal Assertion" << endl;
            fassertFailed( 16278 );
        }
        return JS_TRUE;
    }

    JSBool _MD5( JSContext * cx , JSObject * obj , uintN argc, jsval *argv, jsval *rval ) {
        try {
            if ( argc != 1 ) {
                JS_ReportError( cx , "MD5 needs argument -- MD5(hexstring)" );
                return JS_FALSE;
            }
            Convertor c( cx );
            string s( c.toString( argv[0] ) );
            if ( ! testHexString( cx, s ) ) {
                return JS_FALSE;
            }
            size_t len = s.length();
            if ( len != 32 ) {
                JS_ReportError( cx, "MD5 hexstring length is %d, must be 32", len );
                return JS_FALSE;
            }
            hexToBinData(cx, &c, rval, 5, s);
        }
        catch ( const AssertionException& e ) {
            if ( ! JS_IsExceptionPending( cx ) ) {
                JS_ReportError( cx, e.what() );
            }
            return JS_FALSE;
        }
        catch ( const std::exception& e ) {
            log() << "unhandled exception: " << e.what() << ", throwing Fatal Assertion" << endl;
            fassertFailed( 16279 );
        }
        return JS_TRUE;
    }

    JSBool native_print( JSContext * cx, JSObject * obj, uintN argc, jsval *argv, jsval *rval ) {
        stringstream ss;
        bool someWritten = false;
        try {
            Convertor c( cx );
            for ( uintN i=0; i<argc; i++ ) {
                if ( i > 0 )
                    ss << " ";
                ss << c.toString( argv[i] );
                someWritten = true;
            }
            ss << "\n";
            Logstream::logLockless( ss.str() );
        }
        catch ( const AssertionException& ) {
            if ( someWritten ) {
                ss << "\n";
                Logstream::logLockless( ss.str() );
            }
            return JS_FALSE;
        }
        catch ( const std::exception& e ) {
            log() << "unhandled exception: " << e.what() << ", throwing Fatal Assertion" << endl;
            fassertFailed( 16280 );
        }
        return JS_TRUE;
    }

    JSBool native_helper( JSContext *cx , JSObject *obj , uintN argc, jsval *argv , jsval *rval ) {
        try {
            Convertor c(cx);

            // get function pointer from JS caller's argument property 'x'
            massert(16740, "nativeHelper argument requires object with 'x' property",
                    c.hasProperty(obj, "x"));
            FunctionMap::iterator funcIter = currentScope->_functionMap.find(c.getNumber(obj, "x"));
            massert(16742, "JavaScript function not in map",
                    funcIter != currentScope->_functionMap.end());
            NativeFunction func = funcIter->second;
            verify(func);

            // get data pointer from JS caller's argument property 'y'
            void* data = NULL;
            if (c.hasProperty(obj, "y")) {
                ArgumentMap::iterator argIter =
                        currentScope->_argumentMap.find(c.getNumber(obj, "y"));
                massert(16741, "nativeHelper 'y' parameter must be in the argumentMap",
                         argIter != currentScope->_argumentMap.end());
                data = argIter->second;
            }

            BSONObj a;
            if ( argc > 0 ) {
                BSONObjBuilder args;
                for ( uintN i = 0; i < argc; ++i ) {
                    c.append( args , args.numStr( i ) , argv[i] );
                }
                a = args.obj();
            }

            BSONObj out;
            try {
                out = func( a, data );
            }
            catch ( std::exception& e ) {
                if ( ! JS_IsExceptionPending( cx ) ) {
                    JS_ReportError( cx, e.what() );
                }
                return JS_FALSE;
            }

            if ( out.isEmpty() ) {
                *rval = JSVAL_VOID;
            }
            else {
                *rval = c.toval( out.firstElement() );
            }
        }
        catch ( const AssertionException& e ) {
            if ( ! JS_IsExceptionPending( cx ) ) {
                JS_ReportError( cx, e.what() );
            }
            return JS_FALSE;
        }
        catch ( const std::exception& e ) {
            log() << "unhandled exception: " << e.what() << ", throwing Fatal Assertion" << endl;
            fassertFailed( 16281 );
        }
        return JS_TRUE;
    }

    JSBool native_load( JSContext *cx , JSObject *obj , uintN argc, jsval *argv , jsval *rval );

    JSBool native_gc( JSContext *cx , JSObject *obj , uintN argc, jsval *argv , jsval *rval ) {
        JS_GC( cx );
        return JS_TRUE;
    }

    JSFunctionSpec globalHelpers[] = {
        { "print" , &native_print , 0 , 0 , 0 } ,
        { "nativeHelper" , &native_helper , 1 , 0 , 0 } ,
        { "load" , &native_load , 1 , 0 , 0 } ,
        { "gc" , &native_gc , 1 , 0 , 0 } ,
        { "UUID", &_UUID, 0, 0, 0 } ,
        { "MD5", &_MD5, 0, 0, 0 } ,
        { "HexData", &_HexData, 0, 0, 0 } ,
        { 0 , 0 , 0 , 0 , 0 }
    };

    // ----END global helpers ----

    // Object helpers

    JSBool bson_get_size(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval) {
        try {
            if ( argc != 1 || !JSVAL_IS_OBJECT( argv[ 0 ] ) ) {
                JS_ReportError( cx , "bsonsize requires one valid object" );
                return JS_FALSE;
            }
            Convertor c(cx);
            if ( argv[0] == JSVAL_VOID || argv[0] == JSVAL_NULL ) {
                *rval = c.toval( 0.0 );
                return JS_TRUE;
            }

            JSObject * o = JSVAL_TO_OBJECT( argv[0] );
            double size = 0.0;

            if ( JS_InstanceOf( cx , o , &bson_ro_class , 0 ) ||
                    JS_InstanceOf( cx , o , &bson_class , 0 ) ) {
                BSONHolder * h = GETHOLDER( cx , o );
                if ( h ) {
                    size = h->_obj.objsize();
                }
            }
            else {
                BSONObj temp = c.toObject( o );
                size = temp.objsize();
            }
            *rval = c.toval( size );
        }
        catch ( const AssertionException& e ) {
            if ( ! JS_IsExceptionPending( cx ) ) {
                JS_ReportError( cx, e.what() );
            }
            return JS_FALSE;
        }
        catch ( const std::exception& e ) {
            log() << "unhandled exception: " << e.what() << ", throwing Fatal Assertion" << endl;
            fassertFailed( 16282 );
        }
        return JS_TRUE;
    }

    JSFunctionSpec objectHelpers[] = {
        { "bsonsize" , &bson_get_size , 1 , 0 , 0 } ,
        { 0 , 0 , 0 , 0 , 0 }
    };

    // end Object helpers

    JSBool resolveBSONField( JSContext *cx, JSObject *obj, jsval id, uintN flags, JSObject **objp ) {
        try {
            verify( JS_EnterLocalRootScope( cx ) );
        }
        catch ( const AssertionException& e ) {
            JS_ReportError( cx, e.what() );
            return JS_FALSE;
        }
        catch ( const std::exception& e ) {
            log() << "unhandled exception: " << e.what() << ", throwing Fatal Assertion" << endl;
            fassertFailed( 16283 );
        }

        try {
            BSONHolder * holder = GETHOLDER( cx , obj );
            if ( ! holder ) {
                // static init
                *objp = 0;
                JS_LeaveLocalRootScope( cx );
                return JS_TRUE;
            }
            holder->check();

            Convertor c( cx );
            string s( c.toString( id ) );
            BSONElement e = holder->_obj[ s.c_str() ];
            if ( e.type() == EOO || holder->_removed.count( s ) ) {
                *objp = 0;
                JS_LeaveLocalRootScope( cx );
                return JS_TRUE;
            }

            jsval val = c.toval( e );

            verify( ! holder->_inResolve );
            holder->_inResolve = true;
            verify( JS_SetProperty( cx , obj , s.c_str() , &val ) );
            holder->_inResolve = false;

            if ( val != JSVAL_NULL && val != JSVAL_VOID && JSVAL_IS_OBJECT( val ) ) {
                // TODO: this is a hack to get around sub objects being modified
                // basically right now whenever a sub object is read we mark whole obj as possibly modified
                JSObject * oo = JSVAL_TO_OBJECT( val );
                if ( JS_InstanceOf( cx , oo , &bson_class , 0 ) ||
                        JS_IsArrayObject( cx , oo ) ) {
                    holder->_modified = true;
                }
            }

            *objp = obj;
            JS_LeaveLocalRootScope( cx );
        }
        catch ( const AssertionException& e ) {
            JS_LeaveLocalRootScope( cx );
            if ( ! JS_IsExceptionPending( cx ) ) {
                JS_ReportError( cx, e.what() );
            }
            return JS_FALSE;
        }
        catch ( const std::exception& e ) {
            log() << "unhandled exception: " << e.what() << ", throwing Fatal Assertion" << endl;
            fassertFailed( 16284 );
        }
        return JS_TRUE;
    }


    class SMScope;

    class SMEngine : public ScriptEngine {
    public:

        SMEngine() {
#ifdef SM18
            JS_SetCStringsAreUTF8();
#endif

            _runtime = JS_NewRuntime(64L * 1024L * 1024L);
            uassert( 10221 ,  "JS_NewRuntime failed" , _runtime );

            if ( ! utf8Ok() ) {
                log() << "*** warning: spider monkey build without utf8 support.  consider rebuilding with utf8 support" << endl;
            }
        }

        ~SMEngine() {
            JS_DestroyRuntime( _runtime );
            JS_ShutDown();
        }

        Scope * createScope();

        virtual Scope* newScope() {
            Scope* s = createScope();
            if (!s) return NULL;
            if (_scopeInitCallback)
                _scopeInitCallback(*s);
            installGlobalUtils(*s);
            return s;
        }

        void runTest();

        virtual bool utf8Ok() const { return JS_CStringsAreUTF8(); }

#ifdef XULRUNNER
        JSClass * _dateClass;
        JSClass * _regexClass;
#endif


    private:
        JSRuntime * _runtime;
        friend class SMScope;
    };

    SMEngine * globalSMEngine;


    // ------ scope ------


    JSBool no_gc(JSContext *cx, JSGCStatus status) {
        return JS_FALSE;
    }

    JSBool yes_gc(JSContext *cx, JSGCStatus status) {
        return JS_TRUE;
    }

    SMScope::SMScope() :
            _this( 0 ),
            _reportError( true ),
            _externalSetup( false ),
            _localConnect( false ) {
            smlock;
            _context = JS_NewContext( globalSMEngine->_runtime , 8192 );
            _convertor = new Convertor( _context );
            massert( 10431 ,  "JS_NewContext failed" , _context );

            JS_SetOptions( _context , JSOPTION_VAROBJFIX);
            //JS_SetVersion( _context , JSVERSION_LATEST); TODO
            JS_SetErrorReporter( _context , errorReporter );

            _global = JS_NewObject( _context , &global_class, NULL, NULL);
            massert( 10432 ,  "JS_NewObject failed for global" , _global );
            JS_SetGlobalObject( _context , _global );
            massert( 10433 ,  "js init failed" , JS_InitStandardClasses( _context , _global ) );

            JS_SetOptions( _context , JS_GetOptions( _context ) | JSOPTION_VAROBJFIX );

            JS_DefineFunctions( _context , _global , globalHelpers );

            JS_DefineFunctions( _context , _convertor->getGlobalObject( "Object" ), objectHelpers );

            //JS_SetGCCallback( _context , no_gc ); // this is useful for seeing if something is a gc problem

            _postCreateHacks();
    }

    SMScope::~SMScope() {
            smlock;
            uassert( 10223 ,  "deleted SMScope twice?" , _convertor );

            if ( _this ) {
                JS_RemoveRoot( _context , &_this );
                _this = 0;
            }

            if ( _convertor ) {
                delete _convertor;
                _convertor = 0;
            }

            if ( _context ) {
                // This is expected to reclaim _global as well.
                JS_DestroyContext( _context );
                _context = 0;
            }

    }

    void SMScope::reset() {
            smlock;
            verify( _convertor );
    }

    void SMScope::init( const BSONObj * data ) {
            smlock;
            if ( ! data )
                return;

            BSONObjIterator i( *data );
            while ( i.more() ) {
                BSONElement e = i.next();
                _convertor->setProperty( _global , e.fieldName() , _convertor->toval( e ) );
                _initFieldNames.insert( e.fieldName() );
            }

    }

    bool SMScope::hasOutOfMemoryException() {
            string err = getError();
            return err.find("out of memory") != string::npos;
    }

    void SMScope::externalSetup() {
            smlock;
            uassert( 10224 ,  "already local connected" , ! _localConnect );
            if ( _externalSetup )
                return;
            initMongoJS( this , _context , _global , false );
            _externalSetup = true;
    }

    void SMScope::localConnect( const char * dbName ) {
            {
                smlock;
                uassert( 10225 ,  "already setup for external db" , ! _externalSetup );
                if ( _localConnect ) {
                    uassert( 10226 ,  "connected to different db" , _localDBName == dbName );
                    return;
                }

                initMongoJS( this , _context , _global , true );

                exec( "_mongo = new Mongo();" );
                exec( ((string)"db = _mongo.getDB( \"" + dbName + "\" ); ").c_str() );

                _localConnect = true;
                _localDBName = dbName;
            }
            loadStored();
    }

    double SMScope::getNumber( const char *field ) {
            smlock;
            jsval val;
            verify( JS_GetProperty( _context , _global , field , &val ) );
            return _convertor->toNumber( val );
    }

    string SMScope::getString( const char *field ) {
            smlock;
            jsval val;
            verify( JS_GetProperty( _context , _global , field , &val ) );
            JSString * s = JS_ValueToString( _context , val );
            return _convertor->toString( s );
    }

    bool SMScope::getBoolean( const char *field ) {
            smlock;
            return _convertor->getBoolean( _global , field );
    }

    BSONObj SMScope::getObject( const char *field ) {
            smlock;
            return _convertor->toObject( _convertor->getProperty( _global , field ) );
    }

    JSObject * SMScope::getJSObject( const char * field ) {
            smlock;
            return _convertor->getJSObject( _global , field );
    }

    bool SMScope::isKillPending() const { return globalSMEngine->interrupted(); }

    int SMScope::type( const char *field ) {
            smlock;
            jsval val;
            verify( JS_GetProperty( _context , _global , field , &val ) );

            switch ( JS_TypeOfValue( _context , val ) ) {
            case JSTYPE_VOID: return Undefined;
            case JSTYPE_NULL: return jstNULL;
            case JSTYPE_OBJECT: {
                if ( val == JSVAL_NULL )
                    return jstNULL;
                JSObject * o = JSVAL_TO_OBJECT( val );
                if ( JS_IsArrayObject( _context , o ) )
                    return Array;
                if ( isDate( _context , o ) )
                    return Date;
                return Object;
            }
            case JSTYPE_FUNCTION: return Code;
            case JSTYPE_STRING: return String;
            case JSTYPE_NUMBER: return NumberDouble;
            case JSTYPE_BOOLEAN: return Bool;
            default:
                uassert( 10227 ,  "unknown type" , 0 );
            }
            return 0;
    }

    void SMScope::setElement( const char *field , const BSONElement& val ) {
            smlock;
            jsval v = _convertor->toval( val );
            verify( JS_SetProperty( _context , _global , field , &v ) );
    }

    void SMScope::setNumber( const char *field , double val ) {
            smlock;
            jsval v = _convertor->toval( val );
            verify( JS_SetProperty( _context , _global , field , &v ) );
    }

    void SMScope::setString( const char *field , const StringData& val ) {
            smlock;
            jsval v = _convertor->toval( val.toString().c_str() );
            verify( JS_SetProperty( _context , _global , field , &v ) );
    }

    void SMScope::setObject( const char *field , const BSONObj& obj , bool readOnly ) {
            smlock;
            jsval v = _convertor->toval( &obj , readOnly );
            JS_SetProperty( _context , _global , field , &v );
    }

    void SMScope::setBoolean( const char *field , bool val ) {
            smlock;
            jsval v = BOOLEAN_TO_JSVAL( val );
            verify( JS_SetProperty( _context , _global , field , &v ) );
    }

    void SMScope::setThis( const BSONObj * obj ) {
            smlock;
            if ( _this ) {
                JS_RemoveRoot( _context , &_this );
                _this = 0;
            }

            if ( obj ) {
                _this = _convertor->toJSObject( obj );
                JS_AddNamedRoot( _context , &_this , "scope this" );
            }
    }

    void SMScope::setFunction( const char *field , const char * code ) {
            smlock;
            jsval v = OBJECT_TO_JSVAL(JS_GetFunctionObject(_convertor->compileFunction(code)));
            JS_SetProperty( _context , _global , field , &v );
    }

    void SMScope::rename( const char * from , const char * to ) {
            smlock;
            jsval v;
            verify( JS_GetProperty( _context , _global , from , &v ) );
            verify( JS_SetProperty( _context , _global , to , &v ) );
            v = JSVAL_VOID;
            verify( JS_SetProperty( _context , _global , from , &v ) );
    }

    ScriptingFunction SMScope::_createFunction(const char* code, ScriptingFunction functionNumber) {
            smlock;
            precall();
            return (ScriptingFunction)_convertor->compileFunction( code );
    }

    // should not generate exceptions, as those can be caught in
    // javascript code; returning false without an exception exits
    // immediately
    JSBool SMScope::_interrupt( JSContext *cx ) {
            TimeoutSpec &spec = *(TimeoutSpec *)( JS_GetContextPrivate( cx ) );
            if ( ++spec.count % 1000 != 0 )
                return JS_TRUE;
            const char * interrupt = ScriptEngine::checkInterrupt();
            if ( interrupt && interrupt[ 0 ] ) {
                return JS_FALSE;
            }
            if ( spec.timeout.ticks() == 0 ) {
                return JS_TRUE;
            }
            boost::posix_time::time_duration elapsed = ( boost::posix_time::microsec_clock::local_time() - spec.start );
            if ( elapsed < spec.timeout ) {
                return JS_TRUE;
            }
            return JS_FALSE;

    }

    void SMScope::installInterrupt( int timeoutMs ) {
            if ( timeoutMs != 0 || ScriptEngine::haveCheckInterruptCallback() ) {
                TimeoutSpec *spec = new TimeoutSpec;
                spec->timeout = boost::posix_time::millisec( timeoutMs );
                spec->start = boost::posix_time::microsec_clock::local_time();
                spec->count = 0;
                JS_SetContextPrivate( _context, (void*)spec );
#if defined(SM181) && !defined(XULRUNNER190)
                JS_SetOperationCallback( _context, _interrupt );
#else
                JS_SetBranchCallback( _context, interrupt );
#endif
            }
    }

    void SMScope::uninstallInterrupt( int timeoutMs ) {
            if ( timeoutMs != 0 || ScriptEngine::haveCheckInterruptCallback() ) {
#if defined(SM181) && !defined(XULRUNNER190)
                JS_SetOperationCallback( _context , 0 );
#else
                JS_SetBranchCallback( _context, 0 );
#endif
                delete (TimeoutSpec *)JS_GetContextPrivate( _context );
                JS_SetContextPrivate( _context, 0 );
            }
    }

    void SMScope::precall() {
            _error = "";
            currentScope.reset( this );
    }

    bool SMScope::exec( const StringData& code,
                        const string& name,
                        bool printResult,
                        bool reportError,
                        bool assertOnError,
                        int timeoutMs) {
            smlock;
            precall();

            jsval ret = JSVAL_VOID;

            installInterrupt( timeoutMs );
            _reportError = reportError;
            JSBool worked = JS_EvaluateScript( _context,
                                               _global,
                                               code.rawData(),
                                               code.size(),
                                               name.c_str(),
                                               1,
                                               &ret );
            _reportError = true;
            uninstallInterrupt( timeoutMs );

            if ( ! worked && _error.size() == 0 ) {
                jsval v;
                if ( JS_GetPendingException( _context , &v ) ) {
                    _error = _convertor->toString( v );
                    if ( reportError )
                        cout << _error << endl;
                }
            }

            uassert( 10228, mongoutils::str::stream() << name + " exec failed: " << _error,
                        worked || ! assertOnError );

            if ( worked )
                _convertor->setProperty( _global , "__lastres__" , ret );

            if ( worked && printResult && ! JSVAL_IS_VOID( ret ) )
                cout << _convertor->toString( ret ) << endl;

            return worked;
    }

    int SMScope::invoke( JSFunction* func,
                         const BSONObj* args,
                         const BSONObj* recv,
                         int timeoutMs,
                         bool ignoreReturn,
                         bool readOnlyArgs,
                         bool readOnlyRecv ) {
            smlock;
            precall();

            try {
                verify( JS_EnterLocalRootScope( _context ) );
            }
            catch ( const AssertionException& e ) {
                if ( ! JS_IsExceptionPending( _context ) ) {
                    JS_ReportError( _context, e.what() );
                }
                return 0;
            }
            catch ( const std::exception& e ) {
                log() << "unhandled exception: " << e.what() << ", throwing Fatal Assertion" << endl;
                fassertFailed( 16285 );
            }

            int nargs = args ? args->nFields() : 0;
            scoped_array<jsval> smargsPtr( new jsval[nargs] );
            try {
                if ( nargs ) {
                    BSONObjIterator it( *args );
                    for ( int i=0; i<nargs; i++ ) {
                        smargsPtr[i] = _convertor->toval( it.next() );
                    }
                }

                if ( !args ) {
                    _convertor->setProperty( _global , "args" , JSVAL_NULL );
                }
                else {
                    setObject( "args" , *args , true ); // this is for backwards compatibility
                }
            }
            catch ( const AssertionException& e ) {
                JS_LeaveLocalRootScope( _context );
                if ( ! JS_IsExceptionPending( _context ) ) {
                    JS_ReportError( _context, e.what() );
                }
                return 0;
            }
            catch ( const std::exception& e ) {
                log() << "unhandled exception: " << e.what() << ", throwing Fatal Assertion" << endl;
                fassertFailed( 16286 );
            }
            JS_LeaveLocalRootScope( _context );

            installInterrupt( timeoutMs );
            jsval rval;
            setThis(recv);
            JSBool ret = JS_CallFunction( _context,
                                          _this ? _this : _global,
                                          func,
                                          nargs,
                                          smargsPtr.get(),
                                          &rval );
            setThis(0);
            uninstallInterrupt( timeoutMs );

            if ( !ret ) {
                return -3;
            }

            if ( ! ignoreReturn ) {
                verify( JS_SetProperty( _context , _global , "__returnValue" , &rval ) );
            }

            return 0;
    }

    void SMScope::injectNative( const char *field, NativeFunction func, void* data ) {
            smlock;
            string name = field;
            jsval v;
            uint32_t funcId = _functionMap.size();
            _functionMap.insert(make_pair(funcId, func));
            v = _convertor->toval(static_cast<double>(funcId));
            _convertor->setProperty( _global, (name + "_").c_str(), v );

            stringstream code;
            if (data) {
                uint32_t argsId = _argumentMap.size();
                _argumentMap.insert(make_pair(argsId, data));
                v = _convertor->toval(static_cast<double>(argsId));
                _convertor->setProperty( _global, (name + "_data_").c_str(), v );
                code << field << "_" << " = { x : " << field << "_ , y: " << field << "_data_ }; ";
            } else {
                code << field << "_" << " = { x : " << field << "_ }; ";
            }
            code << field << " = function(){ return nativeHelper.apply( " <<
                                                    field << "_ , arguments ); }";
            exec( code.str() );
    }

    void SMScope::gc() {
            smlock;
            JS_GC( _context );
    }

    void SMScope::_postCreateHacks() {
#ifdef XULRUNNER
            exec( "__x__ = new Date(1);" );
            globalSMEngine->_dateClass = _convertor->getClass( _global , "__x__" );
            exec( "__x__ = /abc/i" );
            globalSMEngine->_regexClass = _convertor->getClass( _global , "__x__" );
#endif
    }

    void errorReporter( JSContext *cx, const char *message, JSErrorReport *report ) {
        stringstream ss;
        if( !isShell )
            ss << "JS Error: ";
        ss << message;

        if ( report && report->filename ) {
            ss << " " << report->filename << ":" << report->lineno;
        }

        if ( !currentScope.get() || currentScope->isReportingErrors() ) {
            tlog() << ss.str() << endl;
        }

        if ( currentScope.get() ) {
            currentScope->gotError( ss.str() );
        }
    }

    JSBool native_load( JSContext *cx , JSObject *obj , uintN argc, jsval *argv , jsval *rval ) {
        try {
            Convertor c(cx);

            Scope * s = currentScope.get();

            for ( uintN i=0; i<argc; i++ ) {
                string filename( c.toString( argv[i] ) );
                //cout << "load [" << filename << "]" << endl;

                if ( ! s->execFile( filename, false, true, false ) ) {
                    JS_ReportError( cx, (string("error loading js file: ") + filename).c_str() );
                    return JS_FALSE;
                }
            }
        }
        catch ( const AssertionException& e ) {
            if ( ! JS_IsExceptionPending( cx ) ) {
                JS_ReportError( cx, e.what() );
            }
            return JS_FALSE;
        }
        catch ( const std::exception& e ) {
            log() << "unhandled exception: " << e.what() << ", throwing Fatal Assertion" << endl;
            fassertFailed( 16287 );
        }
        return JS_TRUE;
    }



    void SMEngine::runTest() {
        SMScope s;

        s.localConnect( "foo" );

        s.exec( "assert( db.getMongo() )" );
        s.exec( "assert( db.bar , 'collection getting does not work' ); " );
        s.exec( "assert.eq( db._name , 'foo' );" );
        s.exec( "assert( _mongo == db.getMongo() ); " );
        s.exec( "assert( _mongo == db._mongo ); " );
        s.exec( "assert( typeof DB.bar == 'undefined' ); " );
        s.exec( "assert( typeof DB.prototype.bar == 'undefined' , 'resolution is happening on prototype, not object' ); " );

        s.exec( "assert( db.bar ); " );
        s.exec( "assert( typeof db.addUser == 'function' )" );
        s.exec( "assert( db.addUser == DB.prototype.addUser )" );
        s.exec( "assert.eq( 'foo.bar' , db.bar._fullName ); " );
        s.exec( "db.bar.verify();" );

        s.exec( "db.bar.silly.verify();" );
        s.exec( "assert.eq( 'foo.bar.silly' , db.bar.silly._fullName )" );
        s.exec( "assert.eq( 'function' , typeof _mongo.find , 'mongo.find is not a function' )" );

        verify( (string)"abc" == trim( "abc" ) );
        verify( (string)"abc" == trim( " abc" ) );
        verify( (string)"abc" == trim( "abc " ) );
        verify( (string)"abc" == trim( " abc " ) );

    }

    Scope * SMEngine::createScope() {
        return new SMScope();
    }

}  // namespace spidermonkey

    void ScriptEngine::setup() {
        spidermonkey::globalSMEngine = new spidermonkey::SMEngine();
        globalScriptEngine = spidermonkey::globalSMEngine;
    }

    std::string ScriptEngine::getInterpreterVersionString() {
        return "SpiderMonkey 1.7";
    }

}  // namespace mongo

