# Copyright 2020 Tarmo Pikaro
# Distributed under the Boost Software License, Version 1.0.
# See accompanying file LICENSE_1_0.txt or copy at https://www.boost.org/LICENSE_1_0.txt

cmake_minimum_required( VERSION 3.5 )
project( BoostDateTime LANGUAGES CXX)

add_library( boost_date_time
    src/gregorian/greg_month.cpp
)
add_library( Boost::date_time ALIAS boost_date_time )

target_include_directories( boost_date_time PUBLIC include )

target_link_libraries( boost_date_time
    PUBLIC
        Boost::config
)



