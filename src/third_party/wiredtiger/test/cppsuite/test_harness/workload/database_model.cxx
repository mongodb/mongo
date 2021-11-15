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

#include <chrono>
#include <tuple>

#include "../connection_manager.h"
#include "../timestamp_manager.h"
#include "../util/api_const.h"
#include "database_model.h"
#include "random_generator.h"
#include "workload_tracking.h"

namespace test_harness {
/* collection class implementation */
collection::collection(const uint64_t id, const uint64_t key_count, const std::string &name)
    : name(name), id(id), _key_count(key_count)
{
}

uint64_t
collection::get_key_count() const
{
    return (_key_count);
}

void
collection::increase_key_count(uint64_t increment)
{
    _key_count += increment;
}

/* database class implementation */
std::string
database::build_collection_name(const uint64_t id)
{
    return (std::string("table:collection_" + std::to_string(id)));
}

void
database::add_collection(uint64_t key_count)
{
    std::lock_guard<std::mutex> lg(_mtx);
    if (_session.get() == nullptr)
        _session = connection_manager::instance().create_session();
    if (_collection_create_config.empty())
        testutil_die(EINVAL, "database_model: no collection create config specified!");
    uint64_t next_id = _next_collection_id++;
    std::string collection_name = build_collection_name(next_id);
    /* FIX-ME-Test-Framework: This will get removed when we split the model up. */
    _collections.emplace(std::piecewise_construct, std::forward_as_tuple(next_id),
      std::forward_as_tuple(next_id, key_count, collection_name));
    testutil_check(
      _session->create(_session.get(), collection_name.c_str(), _collection_create_config.c_str()));
    _tracking->save_schema_operation(
      tracking_operation::CREATE_COLLECTION, next_id, _tsm->get_next_ts());
}

collection &
database::get_collection(uint64_t id)
{
    std::lock_guard<std::mutex> lg(_mtx);
    const auto it = _collections.find(id);
    if (it == _collections.end())
        testutil_die(EINVAL, "tried to get collection that doesn't exist.");
    return (it->second);
}

collection &
database::get_random_collection()
{
    size_t collection_count = get_collection_count();
    /* Any caller should expect at least one collection to exist. */
    testutil_assert(collection_count != 0);
    return (get_collection(
      random_generator::instance().generate_integer<uint64_t>(0, collection_count - 1)));
}

uint64_t
database::get_collection_count()
{
    std::lock_guard<std::mutex> lg(_mtx);
    return (_collections.size());
}

std::vector<std::string>
database::get_collection_names()
{
    std::lock_guard<std::mutex> lg(_mtx);
    std::vector<std::string> collection_names;

    for (auto const &it : _collections)
        collection_names.push_back(it.second.name);

    return (collection_names);
}

std::vector<uint64_t>
database::get_collection_ids()
{
    std::lock_guard<std::mutex> lg(_mtx);
    std::vector<uint64_t> collection_ids;

    for (auto const &it : _collections)
        collection_ids.push_back(it.first);

    return (collection_ids);
}

void
database::set_timestamp_manager(timestamp_manager *tsm)
{
    testutil_assert(_tsm == nullptr);
    _tsm = tsm;
}

void
database::set_workload_tracking(workload_tracking *tracking)
{
    testutil_assert(_tracking == nullptr);
    _tracking = tracking;
}

void
database::set_create_config(bool use_compression)
{
    _collection_create_config = use_compression ?
      DEFAULT_FRAMEWORK_SCHEMA + std::string(SNAPPY_BLK) :
      DEFAULT_FRAMEWORK_SCHEMA;
}
} // namespace test_harness
