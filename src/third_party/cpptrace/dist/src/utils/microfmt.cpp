#include "utils/microfmt.hpp"

#include <iostream>

CPPTRACE_BEGIN_NAMESPACE
namespace microfmt {
namespace detail {

    std::ostream& get_cout() {
        return std::cout;
    }

}
}
CPPTRACE_END_NAMESPACE
