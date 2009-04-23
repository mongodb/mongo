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

    ScriptEngine * globalScriptEngine;
}
