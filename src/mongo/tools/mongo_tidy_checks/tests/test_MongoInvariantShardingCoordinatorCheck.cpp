#include "test_MongoInvariantShardingCoordinatorCheck.h"

namespace mongo {
void MyCoordinator::doSomething() {
    invariant(true);
}

int fun() {
    MyCoordinator c;
    c.doSomething();
    helperFun();
    invariant(true);
    return 0;
}
}  // namespace mongo
