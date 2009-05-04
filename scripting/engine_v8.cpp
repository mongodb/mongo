#include "engine_v8.h"

namespace mongo {

void ScriptEngine::setup(){
    if ( !globalScriptEngine ){
        globalScriptEngine = new V8ScriptEngine();
    }
}

} // namespace mongo