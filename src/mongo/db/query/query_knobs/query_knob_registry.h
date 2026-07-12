// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/init.h"
#include "mongo/db/query/query_knobs/query_knob.h"
#include "mongo/db/query/query_knobs/query_knob_change_notifier.h"
#include "mongo/db/server_parameter.h"
#include "mongo/db/server_parameter_with_storage.h"
#include "mongo/util/modules.h"
#include "mongo/util/string_map.h"
#include "mongo/util/version/releases.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <span>
#include <string_view>
#include <type_traits>
#include <vector>

#include <boost/optional.hpp>

namespace mongo {

/**
 * Process-wide registry of query knobs. Built once at startup and immutable
 * thereafter. Indexed by a dense id for the hot read path, and by wire name
 * for the rare setQuerySettings write path.
 */
class [[MONGO_MOD_PARENT_PRIVATE]] QueryKnobRegistry {
public:
    struct Entry {
        using ReadGlobalFn = std::function<QueryKnobValue()>;
        using FromBSONFn = QueryKnobValue (*)(const BSONElement&);
        using ToBSONFn = void (*)(BSONObjBuilder&, std::string_view, const QueryKnobValue&);
        using AttachOnUpdateFn = std::function<void(const QueryKnobChangeNotifier*)>;
        using AppendTypeFn = void (*)(BSONObjBuilder*);
        using AppendConstraintsFn = std::function<void(BSONObjBuilder*)>;

        template <auto& global, typename T>
        requires AtomicLoadable<decltype(global)>
        static Entry create(QueryKnob<T> knob, std::string_view name) {
            using SPT = IDLServerParameterWithStorage<ServerParameterType::kStartupAndRuntime,
                                                      std::remove_cvref_t<decltype(global)>>;
            return create<SPT>(knob, name);
        }

        template <typename SPT, typename T>
        requires std::derived_from<SPT, ServerParameter>
        static Entry create(QueryKnob<T> knob, std::string_view name) {
            auto* param =
                dynamic_cast<SPT*>(ServerParameterSet::getNodeParameterSet()->getIfExists(name));
            FromBSONFn fromBSON = detail::ConverterTraits<T>::fromBSON;
            ToBSONFn toBSON = detail::ConverterTraits<T>::toBSON;
            AppendTypeFn appendType = detail::ConverterTraits<T>::appendType;
            AppendConstraintsFn appendConstraints = nullptr;
            ReadGlobalFn readGlobal = nullptr;
            AttachOnUpdateFn attachOnUpdate =
                [param, id = knob.id](const QueryKnobChangeNotifier* notifier) {
                    notifier->attach(param, id);
                };
            if constexpr (EnumServerParameter<SPT>) {
                readGlobal = [param]() -> QueryKnobValue {
                    return static_cast<int>(param->_data.get());
                };
                appendConstraints = [](BSONObjBuilder* bob) {
                    // cpp_type parameters do not have standardised validators.
                };
            } else {
                readGlobal = [param]() -> QueryKnobValue {
                    return param->getValue(boost::none /* tenantId */);
                };
                appendConstraints = [param](BSONObjBuilder* bob) {
                    param->appendConstraints(bob);
                };
            }
            return Entry(knob.id,
                         param,
                         fromBSON,
                         toBSON,
                         appendType,
                         appendConstraints,
                         readGlobal,
                         attachOnUpdate);
        }

        explicit Entry(QueryKnobId id,
                       ServerParameter* param,
                       FromBSONFn fromBSONFn,
                       ToBSONFn toBSONFn,
                       AppendTypeFn appendTypeFn,
                       AppendConstraintsFn appendConstraintsFn,
                       ReadGlobalFn readGlobalFn,
                       AttachOnUpdateFn attachOnUpdateFn);

        QueryKnobId id;
        ServerParameter* param = nullptr;

        // Global-value reader and serializers, baked from ConverterTraits<T> at registration.
        FromBSONFn fromBSON = nullptr;
        ToBSONFn toBSON = nullptr;
        AppendTypeFn appendType = nullptr;
        AppendConstraintsFn appendConstraints = nullptr;
        ReadGlobalFn readGlobal = nullptr;
        AttachOnUpdateFn attachOnUpdate = nullptr;

        // Non-owning, points into `param`'s annotation BSON (static lifetime).
        std::string_view wireName;
        bool pqsSettable;
        // Present for PQS knobs only.
        boost::optional<multiversion::FeatureCompatibilityVersion> minFcv;
    };

    QueryKnobRegistry(std::vector<Entry> entries = {});

    static const QueryKnobRegistry& instance();

    /**
     * Initiallizes the process instance. Call once at startup; invariants on a second call.
     */
    static void init(std::vector<Entry> entries);

    /**
     * PQS-settable knobs only; boost::none for non-PQS or unknown name.
     */
    boost::optional<QueryKnobId> getKnobIdForName(std::string_view wireName) const;

    const Entry& entry(QueryKnobId id) const;
    std::span<const Entry> entries() const;

    size_t knobCount() const;
    size_t knobsExposedToQuerySettingsCount() const;

private:
    static QueryKnobRegistry& _mutableInstance();

    std::vector<Entry> _entries;
    StringMap<QueryKnobId> _wireNameIndex;
    size_t _knobsExposedToQuerySettingsCount = 0;
};

namespace detail {
// Invariants if any ServerParameter has a query_knob annotation with a wire_name not found in
// the registry. Catches IDL annotations added without a corresponding QueryKnob<T> declaration.
void detectOrphanAnnotations(const std::vector<QueryKnobRegistry::Entry>& entries,
                             const ServerParameterSet& params);

// Returns the next dense QueryKnobId. Called at static-init time by DEFINE_QUERY_KNOBS.
QueryKnobId allocateQueryKnobId();

// Appends an entry to the global initializer context. Called by REGISTER_QUERY_KNOBS and
// consumed by QueryKnobRegistryInit.
void registerQueryKnob(QueryKnobRegistry::Entry&& entry);
}  // namespace detail

// clang-format off
// See query_knob.h for the full X-macro framework documentation and usage.

// Internal: defines the global QueryKnob<T> handle for each EXPAND row, with a dense id
// freshly allocated from the global QueryKnobInitializer.
#define MONGO_DETAIL_DEFINE_QUERY_KNOB(var, name, global, ...) \
    decltype(var) var{detail::allocateQueryKnobId()};

// Internal: registers one knob into the global QueryKnobInitializer; called inside the
// initializer body.
#define MONGO_DETAIL_INITIALIZE_QUERY_KNOB(var, name, global, ...) \
    detail::registerQueryKnob(QueryKnobRegistry::Entry::create<global>(var, name));

// Internal: emits a MONGO_INITIALIZER that runs after all ServerParameters have been
// registered (so getIfExists() in MONGO_DETAIL_INITIALIZE_QUERY_KNOB sees them) and before
// QueryKnobRegistryInit consumes the builder.
#define MONGO_DETAIL_INITIALIZE_QUERY_KNOBS(group, EXPAND)                                  \
    namespace {                                                                             \
    MONGO_INITIALIZER_GENERAL(group##_init,                                                 \
                              ("EndServerParameterRegistration"),                           \
                              ("QueryKnobRegistryInit"))                                    \
        (InitializerContext*) {                                                             \
            EXPAND(MONGO_DETAIL_INITIALIZE_QUERY_KNOB)                                      \
        }                                                                                   \
    } // namespace

// Defines knob globals and registers them via a MONGO_INITIALIZER. Place in the group .cpp inside
// the knob namespace. GroupName becomes the initializer name (group##_init).
#define REGISTER_QUERY_KNOBS(group, EXPAND)            \
    EXPAND(MONGO_DETAIL_DEFINE_QUERY_KNOB)             \
    MONGO_DETAIL_INITIALIZE_QUERY_KNOBS(group, EXPAND)
// clang-format on
}  // namespace mongo
