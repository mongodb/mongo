#include <boost/unordered_map.hpp>
#include <condition_variable>
#include <future>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace mongo {
void mongoPolyFillCheckTest() {
    std::cv_status wait_result{std::cv_status::timeout};
    if (wait_result == std::cv_status::timeout) {
    }
    std::this_thread::get_id();
    std::get_terminate();
    std::chrono::seconds(1);
    std::chrono::seconds seconds(1);
    std::future<int> myFuture;
    std::condition_variable cv;
    std::unordered_map<int, int> myMap;
    boost::unordered_map<int, int> boostMap;
}

}  // namespace mongo
