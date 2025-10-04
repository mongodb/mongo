#include <atomic>
#include <condition_variable>
#include <future>
#include <mutex>
#include <regex>
#include <thread>
#include <unordered_map>

#include <boost/unordered_map.hpp>

namespace mongo {
void mongoBannedNamesCheckTest() {
    std::get_terminate();
    std::future<int> myFuture;
    std::recursive_mutex recursiveMut;
    const std::condition_variable cv;
    static std::unordered_map<int, int> myMap;
    boost::unordered_map<int, int> boostMap;
    std::atomic<int> atomicVar;
    std::optional<std::string> strOpt;
    std::regex_search(std::string(""), std::regex(""));
}

struct AtomicStruct {
    std::atomic<int> fieldDecl;
};

template <typename T>
std::optional<T> templateDecl;

using std::optional;
}  // namespace mongo
