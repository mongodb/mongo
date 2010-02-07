// common.cpp

#include "../stdafx.h"
#include "concurrency.h"

/**
 * this just has globals
 */
namespace mongo {

    /* we use new here so we don't have to worry about destructor orders at program shutdown */
    MongoMutex &dbMutex( *(new MongoMutex) );

}
