// v8_utils.h

#pragma once

#include <v8.h>

#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <assert.h>
#include <iostream>

namespace mongo {

    // Executes a string within the current v8 context.
    bool ExecuteString(v8::Handle<v8::String> source,
                       v8::Handle<v8::Value> name,
                       bool print_result,
                       bool report_exceptions);
    
    v8::Handle<v8::Value> Print(const v8::Arguments& args);
    void ReportException(v8::TryCatch* handler);
    
    
    void installShellUtils( v8::Handle<v8::ObjectTemplate>& global );
    
#define jsassert(x,msg) assert(x)
    
    std::ostream& operator<<( std::ostream &s, const v8::Handle<v8::Value> & o );
    std::ostream& operator<<( std::ostream &s, const v8::Handle<v8::TryCatch> * try_catch );
    std::string toSTLString( const v8::Handle<v8::Value> & o );

}

