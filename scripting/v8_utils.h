// v8_utils.h

#pragma once

#include <v8.h>

#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <assert.h>
#include <iostream>

namespace mongo {

    v8::Handle<v8::Value> Print(const v8::Arguments& args);
    v8::Handle<v8::Value> Version(const v8::Arguments& args);

    void ReportException(v8::TryCatch* handler);
    
#define jsassert(x,msg) assert(x)
    
    std::ostream& operator<<( std::ostream &s, const v8::Handle<v8::Value> & o );
    std::ostream& operator<<( std::ostream &s, const v8::Handle<v8::TryCatch> * try_catch );
    std::string toSTLString( const v8::Handle<v8::Value> & o );

}

