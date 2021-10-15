/*-
 * Public Domain 2014-present MongoDB, Inc.
 * Public Domain 2008-2014 WiredTiger, Inc.
 *
 * This is free and unencumbered software released into the public domain.
 *
 * Anyone is free to copy, modify, publish, use, compile, sell, or
 * distribute this software, either in source code form or as a compiled
 * binary, for any purpose, commercial or non-commercial, and by any
 * means.
 *
 * In jurisdictions that recognize copyright laws, the author or authors
 * of this software dedicate any and all copyright interest in the
 * software to the public domain. We make this dedication for the benefit
 * of the public at large and to the detriment of our heirs and
 * successors. We intend this dedication to be an overt act of
 * relinquishment in perpetuity of all present and future rights to this
 * software under copyright law.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#ifndef COMPONENT_H
#define COMPONENT_H

#include "configuration.h"
#include "throttle.h"

namespace test_harness {
/*
 * A component is a class that defines 3 unique stages in its life-cycle, the stages must be run in
 * the following order: load, run, finish.
 */
class component {
    public:
    explicit component(const std::string &name, configuration *config);
    virtual ~component();

    /* Delete the copy constructor and the assignment operator. */
    component(const component &) = delete;
    component &operator=(const component &) = delete;

    /*
     * The load function should perform all tasks required to setup the component for the main phase
     * of the test. An example operation performed in the load phase would be populating a database.
     */
    virtual void load();

    /*
     * The run function provides a top level loop that calls the do_work function every X seconds as
     * defined by the throttle. Each run() method defined by the components is called in its own
     * thread by the top level test class.
     *
     * If a component does not wish to use the standard run function, it can be overloaded.
     */
    virtual void run();

    virtual void do_work();

    bool enabled() const;

    /*
     * The finish phase is a cleanup phase. Created objects are destroyed here and any final testing
     * requirements can be performed in this phase. An example could be the verification of the
     * database, or checking some relevant statistics.
     */
    virtual void finish();

    protected:
    bool _enabled = false;
    volatile bool _running = false;
    throttle _throttle;
    configuration *_config;

    private:
    std::string _name;
};
} // namespace test_harness
#endif
