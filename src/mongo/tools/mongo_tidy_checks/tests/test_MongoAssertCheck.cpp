#include <cassert>
namespace mongo {

void testAssertFunction() {
    int x = 10;
    assert(x > 0 && "x should be postive");
}

}  // namespace mongo
