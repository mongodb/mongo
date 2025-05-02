//
// Copyright (c) 2025 Alexander Grund
//
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#include "../src/util/numeric_conversion.hpp"

#include <boost/core/detail/string_view.hpp>
#include <cstdint>
#include <exception>
#include <iostream>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    const boost::core::string_view sv{reinterpret_cast<const char*>(data), size};
    using boost::locale::util::try_scientific_to_int;
    try {
        uint8_t u8{};
        try_scientific_to_int(sv, u8);

        uint16_t u16{};
        try_scientific_to_int(sv, u16);

        uint32_t u32{};
        try_scientific_to_int(sv, u32);

        uint8_t u64{};
        try_scientific_to_int(sv, u64);
    } catch(...) {
        std::cerr << "Error with '" << sv << "' (size " << size << ')' << std::endl;
        std::terminate();
    }

    return 0;
}
