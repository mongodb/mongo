/**
 *    Copyright (C) 2019-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/stdx/mutex.h"

#include <boost/log/detail/fake_mutex.hpp>
#include <boost/log/detail/locking_ptr.hpp>
#include <boost/log/sinks/basic_sink_backend.hpp>
#include <boost/log/sinks/frontend_requirements.hpp>
#include <tuple>

namespace mongo::logv2 {

template <typename... Backend>
class CompositeBackend
    : public boost::log::sinks::basic_formatted_sink_backend<
          char,
          boost::log::sinks::combine_requirements<boost::log::sinks::concurrent_feeding,
                                                  boost::log::sinks::flushing>::type> {
private:
    using filter_func = std::function<bool(boost::log::attribute_value_set const&)>;
    using index_sequence = std::make_index_sequence<sizeof...(Backend)>;

    template <typename backend_t>
    struct BackendTraits {
        // Helper to select mutex type depending on requirements of backend
        using backend_mutex_type = std::conditional_t<
            boost::log::sinks::has_requirement<typename backend_t::frontend_requirements,
                                               boost::log::sinks::concurrent_feeding>::value,
            boost::log::aux::fake_mutex,
            stdx::mutex>;

        BackendTraits(boost::shared_ptr<backend_t> backend) : _backend(std::move(backend)) {}
        boost::shared_ptr<backend_t> _backend;
        backend_mutex_type _mutex;
        std::function<bool(boost::log::attribute_value_set const&)> _filter;
    };

    template <size_t I>
    decltype(auto) getTrait() {
        return std::get<I>(_backendTraits);
    }

public:
    CompositeBackend(boost::shared_ptr<Backend>... backends)
        : _backendTraits(std::move(backends)...) {}

    /**
     * Locking accessor to the attached backend at index
     */
    template <size_t I>
    auto lockedBackend() {
        auto& trait = getTrait<I>();
        return boost::log::aux::locking_ptr(trait._backend, trait._mutex);
    }

    /**
     * Sets post-formatting filter for the attached backend at index
     */
    template <size_t I>
    void setFilter(filter_func filter) {
        getTrait<I>()._filter = std::move(filter);
    }

    /**
     * Resets post-formatting filter for the attached backend at index
     */
    template <size_t I>
    void resetFilter() {
        getTrait<I>()._filter.reset();
    }

    /**
     * Consumes formatted log for all attached backends
     */
    void consume(boost::log::record_view const& rec, string_type const& formatted_string) {
        consumeAll(rec, formatted_string, index_sequence{});
    }

    /**
     * Flushes all attached backends that supports flushing
     */
    void flush() {
        flushAll(index_sequence{});
    }

private:
    template <typename mutex_t, typename backend_t>
    void flushBackend(mutex_t& mutex, backend_t& backend) {
        if constexpr (boost::log::sinks::has_requirement<typename backend_t::frontend_requirements,
                                                         boost::log::sinks::flushing>::value) {
            stdx::lock_guard lock(mutex);

            backend.flush();
        }
    }

    // Helper to flush backend at index
    template <size_t I>
    void flushAt() {
        auto& trait = getTrait<I>();
        flushBackend(trait._mutex, *trait._backend);
    }

    // Helper to expand tuple for flushing backends
    template <size_t... Is>
    void flushAll(std::index_sequence<Is...>) {
        (flushAt<Is>(), ...);
    }

    // Helper to consume logs for backend at index
    template <size_t I>
    void consumeAt(boost::log::record_view const& rec, string_type const& formatted_string) {
        auto& trait = getTrait<I>();
        if (!trait._filter || trait._filter(rec.attribute_values())) {
            stdx::lock_guard lock(trait._mutex);

            trait._backend->consume(rec, formatted_string);
        }
    }

    // Helper to expand tuple for consuming logs
    template <size_t... Is>
    void consumeAll(boost::log::record_view const& rec,
                    string_type const& formatted_string,
                    std::index_sequence<Is...>) {
        (consumeAt<Is>(rec, formatted_string), ...);
    }

    std::tuple<BackendTraits<Backend>...> _backendTraits;
};

}  // namespace mongo::logv2
