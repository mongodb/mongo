#pragma once
#include "db/mongomutex.h"
namespace mongo {
    namespace shell_utils {
        extern mongo::mutex &mongoProgramOutputMutex;
    }
}
