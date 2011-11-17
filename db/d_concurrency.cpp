#include "../pch.h"
#include "database.h"
#include "d_concurrency.h"
#include "../util/concurrency/threadlocal.h"
#include "../util/concurrency/rwlock.h"
#include "client.h"

using namespace std;

namespace mongo {

    LockCollectionForReading::LockCollectionForReading(const char *ns)
    {
//...
    }

}
