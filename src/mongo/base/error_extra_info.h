// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include <memory>
#include <type_traits>

// This file is included by many low-level headers including status.h, so it isn't able to include
// much without creating a cycle.
#include "mongo/base/error_codes.h"
#include "mongo/base/static_assert.h"
#include "mongo/util/modules.h"

namespace [[MONGO_MOD_PUBLIC]] mongo {

class BSONObj;
class BSONObjBuilder;

/**
 * Base class for the extra info that can be attached to commands.
 *
 * Actual implementations must have a 'constexpr ErrorCode::Error code' to indicate which
 * error code they bind to, and a static method with the following signature:
 *      static std::shared_ptr<const ErrorExtraInfo> parse(const BSONObj&);
 * You must call the MONGO_INIT_REGISTER_ERROR_EXTRA_INFO(type) macro in the cpp file that contains
 * the implementation for your subtype.
 */
class [[MONGO_MOD_OPEN]] ErrorExtraInfo {
public:
    using Parser = std::shared_ptr<const ErrorExtraInfo>(const BSONObj&);

    ErrorExtraInfo() = default;
    virtual ~ErrorExtraInfo() = default;

    /**
     * Puts the extra info (and just the extra info) into builder.
     */
    virtual void serialize(BSONObjBuilder* builder) const = 0;

    /**
     * Returns the registered parser for a given code or nullptr if none is registered.
     */
    static Parser* parserFor(ErrorCodes::Error);

    /**
     * Use the MONGO_INIT_REGISTER_ERROR_EXTRA_INFO(type) macro below rather than calling this
     * directly.
     */
    template <typename T>
    static void registerType() {
        MONGO_STATIC_ASSERT(std::is_base_of<ErrorExtraInfo, T>());
        MONGO_STATIC_ASSERT(std::is_same<error_details::ErrorExtraInfoFor<T::code>, T>());
        MONGO_STATIC_ASSERT(std::is_final<T>());
        MONGO_STATIC_ASSERT(std::is_move_constructible<T>());
        registerParser(T::code, T::parse);
    }

    /**
     * Fails fatally if any error codes that should have parsers registered don't. An invariant in
     * this function indicates that there isn't a MONGO_INIT_REGISTER_ERROR_EXTRA_INFO declaration
     * for some error code, which requires an extra info.
     *
     * Call this during startup of any shipping executable to prevent failures at runtime.
     */
    static void invariantHaveAllParsers();

private:
    static void registerParser(ErrorCodes::Error code, Parser* parser);
};

/**
 * Registers the parser for an ErrorExtraInfo subclass. This must be called at namespace scope in
 * the same cpp file as the virtual methods for that type.
 *
 * You must separately #include "mongo/base/init.h" since including it here would create an include
 * cycle.
 */
#define MONGO_INIT_REGISTER_ERROR_EXTRA_INFO(type)                              \
    MONGO_INITIALIZER_GENERAL(RegisterErrorExtraInfoFor##type, (), ("default")) \
    (InitializerContext*) {                                                     \
        ErrorExtraInfo::registerType<type>();                                   \
    }

/**
 * This is an example ErrorExtraInfo subclass. It is used for testing the ErrorExtraInfoHandling.
 *
 * The default parser just throws a numeric code since this class should never be encountered in
 * production. A normal parser is activated while an EnableParserForTesting object is in scope.
 */
class ErrorExtraInfoExample final : public ErrorExtraInfo {
public:
    static constexpr auto code = ErrorCodes::ForTestingErrorExtraInfo;

    void serialize(BSONObjBuilder*) const override;
    static std::shared_ptr<const ErrorExtraInfo> parse(const BSONObj&);

    // Everything else in this class is just for testing and shouldn't by copied by users.

    struct EnableParserForTest {
        EnableParserForTest() {
            isParserEnabledForTest = true;
        }
        ~EnableParserForTest() {
            isParserEnabledForTest = false;
        }
    };

    ErrorExtraInfoExample(int data) : data(data) {}
    int data;  // This uses the fieldname "data".
private:
    static bool isParserEnabledForTest;
};

/**
 * This is an example ErrorExtraInfo subclass. It is used for testing the ErrorExtraInfoHandling.
 *
 * It is meant to be a duplicate of ErrorExtraInfoExample, except that it is optional. This will
 * make sure we don't crash the server when an ErrorExtraInfo class is meant to be optional.
 */
class OptionalErrorExtraInfoExample final : public ErrorExtraInfo {
public:
    static constexpr auto code = ErrorCodes::ForTestingOptionalErrorExtraInfo;

    void serialize(BSONObjBuilder*) const override;
    static std::shared_ptr<const ErrorExtraInfo> parse(const BSONObj&);

    // Everything else in this class is just for testing and shouldn't by copied by users.

    struct EnableParserForTest {
        EnableParserForTest() {
            isParserEnabledForTest = true;
        }
        ~EnableParserForTest() {
            isParserEnabledForTest = false;
        }
    };

    OptionalErrorExtraInfoExample(int data) : data(data) {}
    int data;  // This uses the fieldname "data".
private:
    static bool isParserEnabledForTest;
};


namespace nested::twice {

/**
 * This is an example ErrorExtraInfo subclass. It is used for testing the ErrorExtraInfoHandling.
 *
 * It is meant to be a duplicate of ErrorExtraInfoExample, except that it is within a namespace
 * (and so exercises a different codepath in the parser).
 */
class NestedErrorExtraInfoExample final : public ErrorExtraInfo {
public:
    static constexpr auto code = ErrorCodes::ForTestingErrorExtraInfoWithExtraInfoInNamespace;

    void serialize(BSONObjBuilder*) const override;
    static std::shared_ptr<const ErrorExtraInfo> parse(const BSONObj&);

    // Everything else in this class is just for testing and shouldn't by copied by users.

    struct EnableParserForTest {
        EnableParserForTest() {
            isParserEnabledForTest = true;
        }
        ~EnableParserForTest() {
            isParserEnabledForTest = false;
        }
    };

    NestedErrorExtraInfoExample(int data) : data(data) {}
    int data;  // This uses the fieldname "data".
private:
    static bool isParserEnabledForTest;
};

}  // namespace nested::twice

}  // namespace mongo
