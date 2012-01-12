#pragma once
#include "db/mongomutex.h"
namespace mongo {
    namespace shellUtils {
        extern mongo::mutex &mongoProgramOutputMutex;
    }
}
