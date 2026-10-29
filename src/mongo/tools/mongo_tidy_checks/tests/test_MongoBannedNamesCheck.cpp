#include <atomic>
#include <condition_variable>
#include <future>
#include <mutex>
#include <regex>
#include <string>
#include <thread>
#include <unordered_map>

#include <boost/optional.hpp>
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
    std::jthread t([] {});
}

struct AtomicStruct {
    std::atomic<int> fieldDecl;
};

template <typename T>
std::optional<T> templateDecl;

using std::optional;

std::optional<int> optionalReturn() {
    return {};
}

auto trailingOptionalReturn() -> std::optional<long> {
    return {};
}

void lambdaOptionalReturn() {
    auto lambda = []() -> std::optional<char> {
        return {};
    };
}

void explicitOptionalConstruction() {
    auto ctad = [] {
        return std::optional{5};
    };
    auto functionalCast = [] {
        return std::optional<short>(7);
    };
    // Allowed types: banned-name text in an initializer must not be flagged.
    boost::optional<std::string> allowedDecl{"std::optional"};
    boost::optional<std::string> allowedCopyDecl = std::string("std::optional");
    using boost::optional;
    auto allowed = [] {
        return boost::optional<std::string>{"std::optional"};
    };
}
}  // namespace mongo
