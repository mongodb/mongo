#include "mongo/db/fts/unicode/byte_vector_altivec.h"

#include <array>
#include <atomic>
#include <bit>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

#if !defined(__powerpc64__) || !defined(__LITTLE_ENDIAN__)
#error "The ppc64le cross smoke target must be compiled for ppc64le"
#endif

static_assert(std::endian::native == std::endian::little);

struct alignas(128) Aligned {
    std::mutex mutex;
    std::atomic<int> value{0};
};

bool checkByteVectorMask() {
    using mongo::unicode::ByteVector;
    std::array<ByteVector::Scalar, ByteVector::size> input;
    input.fill(-1);
    for (size_t offset = 0; offset <= input.size(); ++offset) {
        const auto mask = ByteVector::load(input.data()).maskHigh();
        if (mask != (0xFFFFu & (0xFFFFu << offset)) ||
            ByteVector::countInitialZeros(mask) != offset) {
            return false;
        }
        if (offset < input.size()) {
            input[offset] = 0;
        }
    }
    return true;
}

int main() {
    Aligned aligned;
    std::vector<int> values{1, 2, 3};
    {
        std::jthread worker([&] {
            std::lock_guard lock(aligned.mutex);
            aligned.value.fetch_add(values.size());
        });
    }
    std::cout << sizeof(aligned) << " " << values.size() << "\n";
    return sizeof(aligned) == 128 && values.size() == 3 && aligned.value.load() == 3 &&
            checkByteVectorMask()
        ? 0
        : 1;
}
