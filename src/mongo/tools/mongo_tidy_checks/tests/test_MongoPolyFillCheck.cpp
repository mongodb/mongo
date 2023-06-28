#include <boost/unordered_map.hpp>
#include <condition_variable>
#include <future>
#include <mutex>
#include <unordered_map>

namespace mongo {
void mongoPolyFillCheckTest() {
    std::mutex myMutex;
    std::future<int> myFuture;
    std::condition_variable cv;
    std::unordered_map<int, int> myMap;
    boost::unordered_map<int, int> boostMap;
}

}  // namespace mongo
