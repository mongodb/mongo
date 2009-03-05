// ShellUtils.h

#pragma once

#include <v8.h>

#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <assert.h>
#include <iostream>

// Executes a string within the current v8 context.
bool ExecuteString(v8::Handle<v8::String> source,
                   v8::Handle<v8::Value> name,
                   bool print_result,
                   bool report_exceptions);

v8::Handle<v8::Value> Print(const v8::Arguments& args);
v8::Handle<v8::Value> Load(const v8::Arguments& args);
v8::Handle<v8::Value> ListFiles(const v8::Arguments& args);
v8::Handle<v8::Value> Quit(const v8::Arguments& args);
v8::Handle<v8::Value> Version(const v8::Arguments& args);
v8::Handle<v8::Value> JSSleep(const v8::Arguments& args);
v8::Handle<v8::Value> JSFork(const v8::Arguments& args);

v8::Handle<v8::String> ReadFile(const char* name);


void ReportException(v8::TryCatch* handler);


void installShellUtils( v8::Handle<v8::ObjectTemplate>& global );

#define jsassert(x,msg) assert(x)

std::ostream& operator<<( std::ostream &s, const v8::Handle<v8::Value> & o );
std::ostream& operator<<( std::ostream &s, const v8::Handle<v8::TryCatch> * try_catch );
std::string toSTLString( const v8::Handle<v8::Value> & o );

// Scoped management of mongo program instances.  Simple implementation:
// destructor kills all mongod instances created by the shell.
struct MongoProgramScope {
    MongoProgramScope() {} // Avoid 'unused variable' warning.
    ~MongoProgramScope();
};

void RecordMyLocation( const char *_argv0 );
void KillMongoProgramInstances();
