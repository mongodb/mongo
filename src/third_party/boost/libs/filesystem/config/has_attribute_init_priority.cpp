//  Copyright 2021 Andrey Semashev

//  Distributed under the Boost Software License, Version 1.0.
//  See http://www.boost.org/LICENSE_1_0.txt

//  See library home page at http://www.boost.org/libs/filesystem

struct global_class
{
    global_class() {}
    ~global_class() {}
};

__attribute__ ((init_priority(32767)))
global_class g_object;

int main()
{
    return 0;
}
