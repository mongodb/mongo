//
// Copyright (c) 2023 Alexander Grund
//
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#ifndef BOOST_LOCALE_MAKE_STD_UNIQUE_HPP
#define BOOST_LOCALE_MAKE_STD_UNIQUE_HPP

#include <boost/locale/config.hpp>
#include <memory>
#include <type_traits>
#include <utility>

namespace boost { namespace locale {
    template<class T, class... Args>
    std::unique_ptr<T> make_std_unique(Args&&... args)
    {
        static_assert(!std::is_array<T>::value, "Must not be an array");
        return std::unique_ptr<T>(new T(std::forward<Args>(args)...));
    }
}} // namespace boost::locale

#endif
