// server_parameters_inline.h

#include "mongo/util/stringutils.h"

namespace mongo {

    template<typename T>
    inline Status ExportedServerParameter<T>::set( const BSONElement& newValueElement ) {
        T newValue;

        if ( !newValueElement.coerce( &newValue) )
            return Status( ErrorCodes::BadValue, "can't set value" );

        return set( newValue );
    }

    template<typename T>
    inline Status ExportedServerParameter<T>::set( const T& newValue ) {

        Status v = validate( newValue );
        if ( !v.isOK() )
            return v;

        *_value = newValue;
        return Status::OK();
    }

    template<>
    inline Status ExportedServerParameter<int>::setFromString( const string& str ) {
        return set( atoi(str.c_str() ) );
    }

    template<>
    inline Status ExportedServerParameter<string>::setFromString( const string& str ) {
        return set( str );
    }


    template<>
    inline Status ExportedServerParameter< vector<string> >::setFromString( const string& str ) {
        vector<string> v;
        splitStringDelim( str, &v, ',' );
        return set( v );
    }



}
