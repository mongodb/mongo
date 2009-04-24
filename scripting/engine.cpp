// engine.cpp

#include "stdafx.h"
#include "engine.h"

namespace mongo {
    
    Scope::Scope(){
    }

    Scope::~Scope(){
    }

    ScriptEngine::ScriptEngine(){
    }

    ScriptEngine::~ScriptEngine(){
    }

    int Scope::invoke( const char* code , const BSONObj& args ){
        ScriptingFunction func = createFunction( code );
        uassert( "compile failed" , func );
        return invoke( func , args );
    }

    ScriptEngine * globalScriptEngine;
}
