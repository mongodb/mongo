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

#include "component.h"
#include "test_harness/util/api_const.h"

namespace test_harness {
component::component(const std::string &name, configuration *config) : _name(name), _config(config)
{
}

component::~component()
{
    delete _config;
}

void
component::load()
{
    logger::log_msg(LOG_INFO, "Loading component: " + _name);
    _enabled = _config->get_optional_bool(ENABLED, true);
    _throttle = throttle(_config);
    /* If we're not enabled we shouldn't be running. */
    _running = _enabled;
}

void
component::run()
{
    logger::log_msg(LOG_INFO, "Running component: " + _name);
    while (_enabled && _running) {
        do_work();
        _throttle.sleep();
    }
}

void
component::do_work()
{
    /* Not implemented. */
}

bool
component::enabled() const
{
    return (_enabled);
}

void
component::finish()
{
    logger::log_msg(LOG_INFO, "Finishing component: " + _name);
    _running = false;
}
} // namespace test_harness
