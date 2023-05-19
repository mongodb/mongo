//#include "mongo/config.h"

namespace mongo {
#define MONGO_CONFIG_TEST1 1

#ifdef MONGO_CONFIG_TEST1
#endif

#if MONGO_CONFIG_TEST1 == 1
#endif

#ifndef MONGO_CONFIG_TEST2
#endif

#if defined(MONGO_CONFIG_TEST1)
#endif

}  // namespace mongo
