//  Copyright 2021 Andrey Semashev

//  Distributed under the Boost Software License, Version 1.0.
//  See http://www.boost.org/LICENSE_1_0.txt

//  See library home page at http://www.boost.org/libs/filesystem

#include <atomic>

typedef void func_t();

int main()
{
    func_t* func = 0;
    std::atomic_ref< func_t* > ref(func);
    ref.load(std::memory_order_relaxed);
    ref.store((func_t*)0, std::memory_order_relaxed);
    return 0;
}
