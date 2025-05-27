#include "mongo/platform/compiler.h"

#include <condition_variable>
#include <future>
#include <mutex>
#include <unordered_map>

#include <boost/unordered_map.hpp>

namespace mongo {
void mongoNoUniqueAddressTest() {
    struct Dummy {
        [[no_unique_address]] int x;
        MONGO_COMPILER_NO_UNIQUE_ADDRESS int y;
    };
}

}  // namespace mongo
