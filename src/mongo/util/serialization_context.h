// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once
#include "mongo/util/modules.h"
#include "mongo/util/static_immortal.h"
#include "mongo/util/str.h"

#include <boost/optional.hpp>

[[MONGO_MOD_PUBLIC]];

namespace mongo {

/**
 * SerializationContext is a run-time routing mechansim that effectively stores context state
 * consumed by NamespaceStringUtil and DatabaseNameUtil serializer and deserializer wrapper
 * functions in order to modify serialization/deserialization behavior based on caller context. This
 * is mainly used by the IDL generator, and allows us to pass this context through the nested types
 * to ensure it is available to the NamespacedStringUtil and DatabaseNameUtil serializers and
 * deserializers at any nested level.  However, it can also be used wherever code paths converge
 * such that hardcoding the specific serializer or deserializer doesn't make sense.
 *
 * The mechanism by which the IDL generated code passes these flags to its nested structs depends on
 * how the object is being constructed.  If parse() is called, the SerializationContext are stored
 * on the IDLParserContext to propagate to all nested structs, while direct construction requires
 * the SerializationContext to be passed in as constructor params.
 *
 */

struct SerializationContext {
    /*
     * Source is used by the caller to set the caller context, and specifies to the serializer or
     * deserializer in what context it is being called.  The NamespaceStringUtil and
     * DatabaseNameUtil serializers and deserializers can be called from anywhere in the code, so
     * we need to pass this flag in if the serialzation or deserialization behavior depends on
     * where it is being called.  Use default if serialization and deserialization should only
     * depened on the state of the feature flags.
     */
    enum class Source : uint8_t { Default, Command, Storage, Catalog };

    /**
     * The CallerType enum is currently only applicable to source = Command, and is used in
     * combination with this source; it will otherwise be ignored.  This is used to provide the
     * serializers and deserializers with additional context specific to commands and allows us to
     * for example handle serialization for a request differently than serialization for a reply.
     */

    enum class CallerType : uint8_t { None, Request, Reply };

    /**
     * Prefix is used to track whether or not upstream is sending us and thus expecting a prefixed
     * tenant ID in the reply.  This field in the request can either be true, false, or not present,
     * the latter represented by the default state.
     */
    enum class Prefix : uint8_t { IncludePrefix, ExcludePrefix };

    SerializationContext(Source source = Source::Default,
                         CallerType callerType = CallerType::None,
                         Prefix prefixState = Prefix::ExcludePrefix)
        : _source(source), _callerType(callerType), _prefixState(prefixState) {}

    /**
     * Gets a copy of a commonly used immutable value for use in constructing a mutable
     * SerializationContext object
     */
    static const SerializationContext& stateCommandReply() {
        static StaticImmortal<SerializationContext> stateCommandReply{Source::Command,
                                                                      CallerType::Reply};
        return *stateCommandReply;
    }

    static SerializationContext stateCommandReply(const SerializationContext& requestCtxt) {
        return SerializationContext(Source::Command, CallerType::Reply, requestCtxt._prefixState);
    }

    static SerializationContext stateCommandRequest(bool hasTenantId, bool isFromAtlasProxy) {
        return SerializationContext{Source::Command,
                                    CallerType::Request,
                                    isFromAtlasProxy ? Prefix::IncludePrefix
                                                     : Prefix::ExcludePrefix};
    }

    static const SerializationContext& stateCommandRequest() {
        static StaticImmortal<SerializationContext> stateCommandRequest{Source::Command,
                                                                        CallerType::Request};
        return *stateCommandRequest;
    }

    static const SerializationContext& stateStorageRequest() {
        static StaticImmortal<SerializationContext> stateStorageRequest{Source::Storage,
                                                                        CallerType::Request};
        return *stateStorageRequest;
    }

    static const SerializationContext& stateCatalog() {
        static StaticImmortal<SerializationContext> stateCatalog{Source::Catalog, CallerType::None};
        return *stateCatalog;
    }

    static const SerializationContext& stateDefault() {
        static StaticImmortal<SerializationContext> stateDefault{};
        return *stateDefault;
    }

    /**
     * Setters for flags that may not be known during construction time, used by producers
     */


    void setPrefixState(bool prefixState) {
        _prefixState = prefixState ? Prefix::IncludePrefix : Prefix::ExcludePrefix;
    }

    /**
     * Getters for the flags, used by the consumers
     */
    const Source& getSource() const {
        return _source;
    }
    const Prefix& getPrefix() const {
        return _prefixState;
    }
    const CallerType& getCallerType() const {
        return _callerType;
    }

    std::string toString() const {
        auto stream = str::stream();
        stream << "Source: "
               << (_source == Source::Command
                       ? "Command"
                       : (_source == Source::Storage ? "Storage" : "Default"));
        stream << ", CallerType: "
               << (_callerType == CallerType::Request
                       ? "Request"
                       : (_callerType == CallerType::Reply ? "Reply" : "None"));
        stream << ", PrefixState: "
               << (_prefixState == Prefix::IncludePrefix
                       ? "Include"
                       : (_prefixState == Prefix::ExcludePrefix ? "Exclude" : "Missing"));
        return stream;
    }

    friend bool operator==(const SerializationContext& lhs, const SerializationContext& rhs) {
        return (lhs._prefixState == rhs._prefixState) && (lhs._callerType == rhs._callerType) &&
            (lhs._source == rhs._source);
    }

    friend bool operator!=(const SerializationContext& lhs, const SerializationContext& rhs) {
        return !(lhs == rhs);
    }

private:
    Source _source;
    CallerType _callerType;
    Prefix _prefixState;
};

inline std::ostream& operator<<(std::ostream& os, const SerializationContext& sc) {
    return os << sc.toString();
}

}  // namespace mongo
