#include "mongo/s/is_mongos.h"

namespace mongo {
namespace {
bool mongosState = false;
}  // namespace
}  // namespace mongo

bool mongo::isMongos() {
    return mongosState;
}

void mongo::setMongos(const bool state) {
    mongosState = state;
}
