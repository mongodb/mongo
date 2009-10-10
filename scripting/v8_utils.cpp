// ShellUtils.cpp

#include "v8_utils.h"
#include <iostream>
#include <map>
#include <sstream>
#include <vector>
#include <sys/socket.h>
#include <netinet/in.h>

using namespace std;
using namespace v8;

namespace mongo {

    Handle<v8::Value> Print(const Arguments& args) {
        bool first = true;
        for (int i = 0; i < args.Length(); i++) {
            HandleScope handle_scope;
            if (first) {
                first = false;
            } else {
                printf(" ");
            }
            v8::String::Utf8Value str(args[i]);
            printf("%s", *str);
        }
        printf("\n");
        return v8::Undefined();
    }

    std::string toSTLString( const Handle<v8::Value> & o ){
        v8::String::Utf8Value str(o);    
        const char * foo = *str;
        std::string s(foo);
        return s;
    }

    std::ostream& operator<<( std::ostream &s, const Handle<v8::Value> & o ){
        v8::String::Utf8Value str(o);    
        s << *str;
        return s;
    }

    std::ostream& operator<<( std::ostream &s, const v8::TryCatch * try_catch ){
        HandleScope handle_scope;
        v8::String::Utf8Value exception(try_catch->Exception());
        Handle<v8::Message> message = try_catch->Message();
    
        if (message.IsEmpty()) {
            s << *exception << endl;
        } 
        else {

            v8::String::Utf8Value filename(message->GetScriptResourceName());
            int linenum = message->GetLineNumber();
            cout << *filename << ":" << linenum << " " << *exception << endl;

            v8::String::Utf8Value sourceline(message->GetSourceLine());
            cout << *sourceline << endl;

            int start = message->GetStartColumn();
            for (int i = 0; i < start; i++)
                cout << " ";

            int end = message->GetEndColumn();
            for (int i = start; i < end; i++)
                cout << "^";

            cout << endl;
        }    

        if ( try_catch->next_ )
            s << try_catch->next_;

        return s;
    }


    Handle<v8::Value> Version(const Arguments& args) {
        return v8::String::New(v8::V8::GetVersion());
    }

    bool ExecuteString(Handle<v8::String> source, Handle<v8::Value> name,
                       bool print_result, bool report_exceptions ){
    
        HandleScope handle_scope;
        v8::TryCatch try_catch;
    
        Handle<v8::Script> script = v8::Script::Compile(source, name);
        if (script.IsEmpty()) {
            if (report_exceptions)
                ReportException(&try_catch);
            return false;
        } 
    
        Handle<v8::Value> result = script->Run();
        if ( result.IsEmpty() ){
            if (report_exceptions)
                ReportException(&try_catch);
            return false;
        } 
    
        if ( print_result ){
        
            Local<Context> current = Context::GetCurrent();
            Local<Object> global = current->Global();
        
            Local<Value> shellPrint = global->Get( String::New( "shellPrint" ) );

            if ( shellPrint->IsFunction() ){
                v8::Function * f = (v8::Function*)(*shellPrint);
                Handle<v8::Value> argv[1];
                argv[0] = result;
                f->Call( global , 1 , argv );
            }
            else if ( ! result->IsUndefined() ){
                cout << result << endl;
            }
        }
    
        return true;
    }

    void ReportException(v8::TryCatch* try_catch) {
        cout << try_catch << endl;
    }

    extern v8::Handle< v8::Context > baseContext_;
    
    void installShellUtils( Handle<v8::ObjectTemplate>& global ){
        global->Set(v8::String::New("print"), v8::FunctionTemplate::New(Print));
        global->Set(v8::String::New("version"), v8::FunctionTemplate::New(Version));
    }

}
