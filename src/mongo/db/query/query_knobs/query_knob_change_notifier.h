/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/base/status.h"
#include "mongo/db/query/query_knobs/query_knob.h"
#include "mongo/util/functional.h"
#include "mongo/util/intrusive_counter.h"

#include <type_traits>
#include <vector>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

template <typename T, typename U>
concept HasOnUpdateHook =
    requires(T t) { t.setOnUpdate(std::declval<std::function<Status(const U&)>>()); };

struct QueryKnobChange {
    QueryKnobId id;
    QueryKnobValue newValue;
};

class QueryKnobChangeNotifier : public RefCountable {
public:
    using Listener = unique_function<Status(const QueryKnobChange&)>;

    static boost::intrusive_ptr<const QueryKnobChangeNotifier> create(
        std::vector<Listener>&& listeners);

    template <typename SPT>
    requires std::derived_from<SPT, ServerParameter>
    void attach(SPT* sp, QueryKnobId id) const {
        // Skip attaching the hook if no listeners are registered.
        if (_listeners.empty()) {
            return;
        }

        if constexpr (EnumServerParameter<SPT>) {
            using KnobValueType = decltype(detail::queryKnobValueType<SPT>());
            static_assert(HasOnUpdateHook<decltype(sp->_data), KnobValueType>,
                          "Enum query knobs must use a data type with an on-update hook");
            sp->_data.setOnUpdate([this, id, anchor = anchor()](const auto& newVal) {
                static_assert(std::is_enum_v<KnobValueType>);
                return fireEvent({.id = id, .newValue = static_cast<int>(newVal)});
            });
        } else {
            // IDL server parameters types have built-in on-update hooks. Attach directly.
            sp->setOnUpdate([this, id, anchor = anchor()](const auto& newVal) {
                return fireEvent({.id = id, .newValue = newVal});
            });
        }
    }

    Status fireEvent(const QueryKnobChange& event) const;

private:
    QueryKnobChangeNotifier(std::vector<Listener>&& listeners);

    boost::intrusive_ptr<const QueryKnobChangeNotifier> anchor() const;

    std::vector<Listener> _listeners;
};

namespace detail {
void registerQueryKnobListener(QueryKnobChangeNotifier::Listener&& listener);
}  // namespace detail

// clang-format off
// Registers a knob-change listener. `name` is the initializer name (must be a unique
// identifier); `listener` is any expression convertible to QueryKnobChangeNotifier::Listener
// (Status(const QueryKnobChange&)). Place at namespace scope in a .cpp.
#define REGISTER_QUERY_KNOBS_LISTENER(name, listener)                                           \
    namespace {                                                                                 \
    MONGO_INITIALIZER_GENERAL(name,                                                              \
                              ("BeginQueryKnobChangeListenerRegistration"),                      \
                              ("EndQueryKnobChangeListenerRegistration"))                        \
        (InitializerContext*) {                                                                 \
            detail::registerQueryKnobListener(listener);                                        \
        }                                                                                       \
    } // namespace
// clang-format on
}  // namespace mongo
