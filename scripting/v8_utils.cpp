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

    std::string toSTLString( const v8::TryCatch * try_catch ){
        
        stringstream ss;
        
        while ( try_catch ){
            
            v8::String::Utf8Value exception(try_catch->Exception());
            Handle<v8::Message> message = try_catch->Message();
            
            if (message.IsEmpty()) {
                ss << *exception << endl;
            } 
            else {
                
                v8::String::Utf8Value filename(message->GetScriptResourceName());
                int linenum = message->GetLineNumber();
                ss << *filename << ":" << linenum << " " << *exception << endl;
                
                v8::String::Utf8Value sourceline(message->GetSourceLine());
                ss << *sourceline << endl;
                
                int start = message->GetStartColumn();
                for (int i = 0; i < start; i++)
                    ss << " ";
                
                int end = message->GetEndColumn();
                for (int i = start; i < end; i++)
                    ss << "^";
                
                ss << endl;
            }    
            
            try_catch = try_catch->next_;
        }
        
        return ss.str();
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

    void ReportException(v8::TryCatch* try_catch) {
        cout << try_catch << endl;
    }

}
