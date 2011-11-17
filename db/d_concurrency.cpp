#include "../pch.h"
#include "database.h"
#include "d_concurrency.h"
#include "../util/concurrency/threadlocal.h"
#include "../util/concurrency/rwlock.h"
#include "client.h"

using namespace std;

namespace mongo {

#if defined(CLC)
   
    static void dbLock(Client& c) {
        Client::LockStatus& s = c.lockStatus;
        if( s.db++ == 0 ) { // i.e. allow recursive
        }
    }

    LockCollectionForReading::LockCollectionForReading(const char *ns)
    {
        Client& c = cc();
        lockDb(c);


//...
    }

#endif

}
