// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
