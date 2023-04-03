/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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
#include <boost/optional.hpp>

#include "mongo/util/static_immortal.h"

#include "mongo/util/static_immortal.h"

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
    enum class Source { Default, Command, Storage } _source;

    /**
     * The CallerType enum is currently only applicable to source = Command, and is used in
     * combination with this source; it will otherwise be ignored.  This is used to provide the
     * serializers and deserializers with additional context specific to commands and allows us to
     * for example handle serialization for a request differently than serialization for a reply.
     */

    enum class CallerType { None, Request, Reply } _callerType;

    /**
     * Prefix is used to track whether or not upstream is sending us and thus expecting a prefixed
     * tenant ID in the reply.  This field in the request can either be true, false, or not present,
     * the latter represented by the default state.
     */
    enum class Prefix { Default, IncludePrefix, ExcludePrefix } _prefixState;

    SerializationContext(Source source = Source::Default,
                         CallerType callerType = CallerType::None,
                         Prefix prefixState = Prefix::Default)
        : _source(source), _callerType(callerType), _prefixState(prefixState) {}

    static const SerializationContext& stateCommandReply() {
        static StaticImmortal<SerializationContext> stateCommandReply{Source::Command,
                                                                      CallerType::Reply};
        return *stateCommandReply;
    }

    static const SerializationContext& stateCommandRequest() {
        static StaticImmortal<SerializationContext> stateCommandRequest{Source::Command,
                                                                        CallerType::Request};
        return *stateCommandRequest;
    }

    void setPrefixState(bool prefixState) {
        _prefixState = prefixState ? Prefix::IncludePrefix : Prefix::ExcludePrefix;
    }
};

}  // namespace mongo
