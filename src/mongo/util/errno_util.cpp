// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/util/errno_util.h"

#include <string_view>
#include <system_error>

#include <fmt/format.h>

#ifdef _WIN32
#include <errhandlingapi.h>
#include <winsock2.h>
#endif

#if !defined(_WIN32) && !defined(__wasi__)
#include <netdb.h>
#endif

namespace mongo {
using namespace std::literals::string_view_literals;

#ifdef _WIN32
namespace errno_util_win32_detail {
int gle() {
    return GetLastError();
}
int wsaGle() {
    return WSAGetLastError();
}
}  // namespace errno_util_win32_detail
#endif

class AddrInfoErrorCategory : public std::error_category {
public:
    const char* name() const noexcept override {
        return "getaddrinfo";
    }

    std::string message(int e) const override {
#ifdef _WIN32
        return systemError(e).message();
#elif defined(__wasi__)
        return "getaddrinfo error " + std::to_string(e) + " (networking not supported in WASI)";
#else
        return gai_strerror(e);
#endif
    }
};

const std::error_category& addrInfoCategory() {
    static auto p = new AddrInfoErrorCategory;
    return *p;
}

std::string errorMessage(std::error_code ec) {
    std::string r = ec.message();
    bool vague = false;
#if defined(_WIN32)
    vague = (r == "unknown error"sv);
#elif defined(_LIBCPP_VERSION)
    vague = std::string_view{r}.starts_with("unspecified"sv);
#endif
    if (vague)
        return fmt::format("Unknown error {}", ec.value());
    return r;
}

}  // namespace mongo
