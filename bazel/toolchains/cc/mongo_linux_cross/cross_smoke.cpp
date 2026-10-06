#include <atomic>
#include <iostream>
#include <mutex>
#include <vector>

#ifndef __s390x__
#error "The s390x cross smoke target must be compiled for s390x"
#endif

struct alignas(256) Aligned {
    std::mutex mutex;
    std::atomic<int> value{0};
};

int main() {
    Aligned aligned;
    std::vector<int> values{1, 2, 3};
    std::cout << sizeof(aligned) << " " << values.size() << "\n";
    return sizeof(aligned) == 256 && values.size() == 3 ? 0 : 1;
}
