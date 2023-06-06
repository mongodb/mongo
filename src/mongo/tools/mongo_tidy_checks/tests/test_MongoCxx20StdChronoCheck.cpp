/**
 * Forward-declaring C++20 types from std::chrono, because this test is currently compiled with
 * -std=c++17. This trick lets us test the new clang-tidy rule without changing the C++ standard,
 * but we are limited to testing only the pointer types.
 * TODO SERVER-77406: Compile clang-tidy tests with -std=c++20 and add tests for non-pointer types.
 */
namespace std {
namespace chrono {
class day;
class month;
class month_day;
class year;
}  // namespace chrono
}  // namespace std

namespace mongo {

// Decorated types in variable declarations.
std::chrono::day* d1;
const std::chrono::day* d2;

// Source types in 'typedefs' and 'using'.
typedef std::chrono::month month_t;
using year_t = std::chrono::year;

// Types of function parameters and return values.
std::chrono::month_day* getLastDay(std::chrono::month* value);

// Types in data member (field) declarations.
struct DayWrapper {
    std::chrono::day* d3;
};

// Types without full qualification.
using namespace std::chrono;
day* d4;
}  // namespace mongo
