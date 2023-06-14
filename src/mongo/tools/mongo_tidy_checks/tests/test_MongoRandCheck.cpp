#include <cstdlib>
#include <ctime>
namespace mongo {
void mongoRandCheck() {
    srand(time(0));
    int random_number = rand();
}
}  // namespace mongo
