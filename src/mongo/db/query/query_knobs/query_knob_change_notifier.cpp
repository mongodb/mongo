// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/query_knobs/query_knob_change_notifier.h"

#include "mongo/db/query/query_knobs/query_knob_registry.h"

#include <memory>
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

// Owns the process-wide notifier and drives its startup initialization.
class QueryKnobChangeNotifierInitializer {
public:
    void addListener(QueryKnobChangeNotifier::Listener&& listener) {
        _listeners.push_back(std::move(listener));
    }

    // Builds the notifier, attaches it to every knob's on-update hook, and records a baseline of
    // current values before the startup options handling window.
    void buildAndAttach() {
        _notifier = QueryKnobChangeNotifier::create(std::move(_listeners));
        auto&& entries = QueryKnobRegistry::instance().entries();
        _baseline.reserve(entries.size());
        for (auto&& entry : entries) {
            entry.attachOnUpdate(_notifier.get());
            _baseline.push_back(entry.readGlobal());
        }
    }

    // Fires a change event for any knob overriden directly during the startup options handling
    // window.
    void replayDirectOverrides() {
        invariant(_notifier);
        auto&& entries = QueryKnobRegistry::instance().entries();
        for (size_t i = 0; i < entries.size(); ++i) {
            auto current = entries[i].readGlobal();
            if (current != _baseline[i]) {
                uassertStatusOK(_notifier->fireEvent({.id = entries[i].id, .newValue = current}));
            }
        }
    }

private:
    boost::intrusive_ptr<const QueryKnobChangeNotifier> _notifier;

    std::vector<QueryKnobChangeNotifier::Listener> _listeners;

    // Knob values at attach time, indexed by registry position.
    std::vector<QueryKnobValue> _baseline;
};

std::unique_ptr<QueryKnobChangeNotifierInitializer> gInitializer;

// Opens the listener registration window; REGISTER_QUERY_KNOBS_LISTENER slots between Begin and
// End.
MONGO_INITIALIZER_GENERAL(BeginQueryKnobChangeListenerRegistration,
                          ("QueryKnobRegistryInit"),
                          ("EndQueryKnobChangeListenerRegistration"))(InitializerContext*) {
    gInitializer = std::make_unique<QueryKnobChangeNotifierInitializer>();
};

// Closes the registration window and builds the notifier, before startup options are applied.
MONGO_INITIALIZER_GENERAL(EndQueryKnobChangeListenerRegistration,
                          ("BeginQueryKnobChangeListenerRegistration"),
                          ("BeginStartupOptionHandling"))(InitializerContext*) {
    gInitializer->buildAndAttach();
};

// Replays overrides applied outside the ServerParameter setter, then tears down the initializer;
// the notifier stays alive through its attached hooks.
MONGO_INITIALIZER_GENERAL(ReconcileQueryKnobStartupChangeEvents,
                          ("EndQueryKnobChangeListenerRegistration", "EndStartupOptionHandling"),
                          ())(InitializerContext*) {
    gInitializer->replayDirectOverrides();
    gInitializer.reset();
};
}  // namespace

void registerQueryKnobListener(QueryKnobChangeNotifier::Listener&& listener) {
    gInitializer->addListener(std::move(listener));
}
}  // namespace detail
}  // namespace mongo
