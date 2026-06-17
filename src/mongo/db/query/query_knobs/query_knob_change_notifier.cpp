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

#include "mongo/db/query/query_knobs/query_knob_change_notifier.h"

#include "mongo/db/query/query_knobs/query_knob_registry.h"

#include <utility>

namespace mongo {

boost::intrusive_ptr<const QueryKnobChangeNotifier> QueryKnobChangeNotifier::create(
    std::vector<Listener>&& listeners) {
    return new QueryKnobChangeNotifier(std::move(listeners));
}

QueryKnobChangeNotifier::QueryKnobChangeNotifier(std::vector<Listener>&& listeners)
    : _listeners(std::move(listeners)) {}

boost::intrusive_ptr<const QueryKnobChangeNotifier> QueryKnobChangeNotifier::anchor() const {
    return this;
}

Status QueryKnobChangeNotifier::fireEvent(const QueryKnobChange& event) const {
    for (const auto& listener : _listeners) {
        if (auto status = listener(event); !status.isOK()) {
            return status;
        }
    }
    return Status::OK();
}

namespace detail {
namespace {
std::vector<QueryKnobChangeNotifier::Listener> gRegisteredListeners;

MONGO_INITIALIZER_WITH_PREREQUISITES(QueryKnobChangeNotifierInit,
                                     ("QueryKnobRegistryInit"))(InitializerContext*) {
    auto notifier = QueryKnobChangeNotifier::create(std::move(detail::gRegisteredListeners));
    for (auto&& entry : QueryKnobRegistry::instance().entries()) {
        entry.attachOnUpdate(notifier.get());
    }
};
}  // namespace

void registerQueryKnobListener(QueryKnobChangeNotifier::Listener&& listener) {
    gRegisteredListeners.push_back(std::move(listener));
}
}  // namespace detail
}  // namespace mongo
