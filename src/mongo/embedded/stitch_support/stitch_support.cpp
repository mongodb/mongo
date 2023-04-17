/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "stitch_support/stitch_support.h"

#include "api_common.h"
#include "mongo/base/initializer.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/concurrency/locker_noop_client_observer.h"
#include "mongo/db/exec/projection_executor.h"
#include "mongo/db/exec/projection_executor_builder.h"
#include "mongo/db/matcher/matcher.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/ops/parsed_update_array_filters.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/query/projection_parser.h"
#include "mongo/db/service_context.h"
#include "mongo/db/update/update_driver.h"
#include "mongo/util/assert_util.h"

#include <algorithm>
#include <memory>
#include <string>

#if defined(_WIN32)
#define MONGO_API_CALL __cdecl
#else
#define MONGO_API_CALL
#endif

namespace mongo {

using StitchSupportStatusImpl = StatusForAPI<stitch_support_v1_error>;

const NamespaceString kDummyNamespaceStr = NamespaceString("");

/**
 * C interfaces that use enterCXX() must provide a translateException() function that converts any
 * possible exception into a StatusForAPI<> object.
 */
static StitchSupportStatusImpl translateException(
    stdx::type_identity<StitchSupportStatusImpl>) try {
    throw;
} catch (const ExceptionFor<ErrorCodes::ReentrancyNotAllowed>& ex) {
    return {STITCH_SUPPORT_V1_ERROR_REENTRANCY_NOT_ALLOWED, ex.code(), ex.what()};
} catch (const DBException& ex) {
    return {STITCH_SUPPORT_V1_ERROR_EXCEPTION, ex.code(), ex.what()};
} catch (const ExceptionForAPI<stitch_support_v1_error>& ex) {
    return {ex.statusCode(), mongo::ErrorCodes::InternalError, ex.what()};
} catch (const std::bad_alloc& ex) {
    return {STITCH_SUPPORT_V1_ERROR_ENOMEM, mongo::ErrorCodes::InternalError, ex.what()};
} catch (const std::exception& ex) {
    return {STITCH_SUPPORT_V1_ERROR_UNKNOWN, mongo::ErrorCodes::InternalError, ex.what()};
} catch (...) {
    return {STITCH_SUPPORT_V1_ERROR_UNKNOWN,
            mongo::ErrorCodes::InternalError,
            "Unknown error encountered in performing requested stitch_support_v1 operation"};
}

/**
 * C interfaces that use enterCXX() must provide a tranlsateExceptionFallback() function that
 * populates a StatusForAPI<> object to indicate a double-fault error during error reporting. The
 * translateExceptionFallback() function gets called when translateException() throws, and it should
 * not include any code that may itself throw.
 *
 * We use an out param instead of returning the StatusForAPI<> object so as to avoid a std::string
 * copy that may allocate memory.
 */
static void translateExceptionFallback(StitchSupportStatusImpl& status) noexcept {
    status.error = STITCH_SUPPORT_V1_ERROR_IN_REPORTING_ERROR;
    status.exception_code = -1;
    setErrorMessageNoAlloc(status.what);
}

}  // namespace mongo

struct stitch_support_v1_status {
    mongo::StitchSupportStatusImpl statusImpl;
};

namespace mongo {
namespace {

StitchSupportStatusImpl* getStatusImpl(stitch_support_v1_status* status) {
    return status ? &status->statusImpl : nullptr;
}

using StitchSupportException = ExceptionForAPI<stitch_support_v1_error>;

ServiceContext* initialize() {
    srand(static_cast<unsigned>(curTimeMicros64()));  // NOLINT

    // The global initializers can take arguments, which would normally be supplied on the command
    // line, but we assume that clients of this library will never want anything other than the
    // defaults for all configuration that would be controlled by these parameters.
    Status status = mongo::runGlobalInitializers(std::vector<std::string>{});
    uassertStatusOKWithContext(status, "Global initialization failed");
    setGlobalServiceContext(ServiceContext::make());
    auto serviceContext = getGlobalServiceContext();
    serviceContext->registerClientObserver(std::make_unique<LockerNoopClientObserver>());

    return serviceContext;
}

struct ServiceContextDestructor {
    /**
     * This destructor gets called when the Stitch Library gets torn down, either by a call to
     * stitch_support_v1_fini() or when the process exits.
     */
    void operator()(mongo::ServiceContext* const serviceContext) const noexcept {
        Status status = mongo::runGlobalDeinitializers();
        uassertStatusOKWithContext(status, "Global deinitilization failed");

        setGlobalServiceContext(nullptr);
    }
};

using EmbeddedServiceContextPtr = std::unique_ptr<mongo::ServiceContext, ServiceContextDestructor>;
}  // namespace
}  // namespace mongo

struct stitch_support_v1_lib {
    stitch_support_v1_lib() : serviceContext(mongo::initialize()) {}

    stitch_support_v1_lib(const stitch_support_v1_lib&) = delete;
    void operator=(const stitch_support_v1_lib&) = delete;

    mongo::EmbeddedServiceContextPtr serviceContext;
};

struct stitch_support_v1_collator {
    stitch_support_v1_collator(std::unique_ptr<mongo::CollatorInterface> collator)
        : collator(std::move(collator)) {}
    std::unique_ptr<mongo::CollatorInterface> collator;
};

struct stitch_support_v1_matcher {
    stitch_support_v1_matcher(mongo::ServiceContext::UniqueClient client,
                              const mongo::BSONObj& filterBSON,
                              stitch_support_v1_collator* collator)
        : client(std::move(client)),
          opCtx(this->client->makeOperationContext()),
          expCtx(new mongo::ExpressionContext(opCtx.get(),
                                              collator ? collator->collator->clone() : nullptr,
                                              mongo::kDummyNamespaceStr)),
          matcher(filterBSON.getOwned(), expCtx){};

    mongo::ServiceContext::UniqueClient client;
    mongo::ServiceContext::UniqueOperationContext opCtx;
    boost::intrusive_ptr<mongo::ExpressionContext> expCtx;
    mongo::Matcher matcher;
};

struct stitch_support_v1_projection {
    stitch_support_v1_projection(mongo::ServiceContext::UniqueClient client,
                                 const mongo::BSONObj& pattern,
                                 stitch_support_v1_matcher* matcher,
                                 stitch_support_v1_collator* collator)
        : client(std::move(client)), opCtx(this->client->makeOperationContext()), matcher(matcher) {

        auto expCtx = mongo::make_intrusive<mongo::ExpressionContext>(
            opCtx.get(),
            collator ? collator->collator->clone() : nullptr,
            mongo::kDummyNamespaceStr);
        const auto policies = mongo::ProjectionPolicies::findProjectionPolicies();
        auto proj = mongo::projection_ast::parseAndAnalyze(
            expCtx,
            pattern,
            matcher ? matcher->matcher.getMatchExpression() : nullptr,
            matcher ? *matcher->matcher.getQuery() : mongo::BSONObj(),
            policies);

        uassert(51051,
                "$textScore, $sortKey, $recordId and $geoNear are not allowed in this "
                "context",
                !proj.metadataDeps().any());

        this->requiresMatch = proj.requiresMatchDetails();
        this->projectionExec = mongo::projection_executor::buildProjectionExecutor(
            expCtx, &proj, policies, mongo::projection_executor::kDefaultBuilderParams);
    }

    mongo::ServiceContext::UniqueClient client;
    mongo::ServiceContext::UniqueOperationContext opCtx;
    std::unique_ptr<mongo::projection_executor::ProjectionExecutor> projectionExec;

    bool requiresMatch = false;
    stitch_support_v1_matcher* matcher;
};

struct stitch_support_v1_update_details {
    std::vector<std::string> modifiedPaths;
};

struct stitch_support_v1_update {
    stitch_support_v1_update(mongo::ServiceContext::UniqueClient client,
                             mongo::BSONObj updateExpr,
                             mongo::BSONArray arrayFilters,
                             stitch_support_v1_matcher* matcher,
                             stitch_support_v1_collator* collator)
        : client(std::move(client)),
          opCtx(this->client->makeOperationContext()),
          expCtx(new mongo::ExpressionContext(opCtx.get(),
                                              collator ? collator->collator->clone() : nullptr,
                                              mongo::kDummyNamespaceStr)),
          updateExpr(updateExpr.getOwned()),
          arrayFilters(arrayFilters.getOwned()),
          matcher(matcher),
          updateDriver(expCtx) {
        std::vector<mongo::BSONObj> arrayFilterVector;
        for (auto&& filter : this->arrayFilters) {
            arrayFilterVector.push_back(filter.embeddedObject());
        }
        this->parsedFilters = uassertStatusOK(
            mongo::parsedUpdateArrayFilters(expCtx, arrayFilterVector, mongo::kDummyNamespaceStr));

        updateDriver.parse(
            mongo::write_ops::UpdateModification::parseFromClassicUpdate(this->updateExpr),
            parsedFilters);

        uassert(51037,
                "Updates with a positional operator require a matcher object.",
                matcher || !updateDriver.needMatchDetails());
    }

    mongo::ServiceContext::UniqueClient client;
    mongo::ServiceContext::UniqueOperationContext opCtx;
    boost::intrusive_ptr<mongo::ExpressionContext> expCtx;
    mongo::BSONObj updateExpr;
    mongo::BSONArray arrayFilters;

    stitch_support_v1_matcher* matcher;

    std::map<mongo::StringData, std::unique_ptr<mongo::ExpressionWithPlaceholder>> parsedFilters;
    mongo::UpdateDriver updateDriver;
};

namespace mongo {
namespace {

std::unique_ptr<stitch_support_v1_lib> library;

stitch_support_v1_lib* stitch_lib_init() {
    if (library) {
        throw StitchSupportException{
            STITCH_SUPPORT_V1_ERROR_LIBRARY_ALREADY_INITIALIZED,
            "Cannot initialize the Stitch Support Library when it is already initialized."};
    }

    library = std::make_unique<stitch_support_v1_lib>();

    return library.get();
}

void stitch_lib_fini(stitch_support_v1_lib* const lib) {
    if (!lib) {
        throw StitchSupportException{
            STITCH_SUPPORT_V1_ERROR_INVALID_LIB_HANDLE,
            "Cannot close a `NULL` pointer referencing a Stitch Support Library Instance"};
    }

    if (!library) {
        throw StitchSupportException{
            STITCH_SUPPORT_V1_ERROR_LIBRARY_NOT_INITIALIZED,
            "Cannot close the Stitch Support Library when it is not initialized"};
    }

    if (library.get() != lib) {
        throw StitchSupportException{STITCH_SUPPORT_V1_ERROR_INVALID_LIB_HANDLE,
                                     "Invalid Stitch Support Library handle."};
    }

    library.reset();
}

stitch_support_v1_collator* collator_create(stitch_support_v1_lib* const lib,
                                            BSONObj collationSpecExpr) {
    if (!library) {
        throw StitchSupportException{STITCH_SUPPORT_V1_ERROR_LIBRARY_NOT_INITIALIZED,
                                     "Cannot create a new collator when the Stitch Support Library "
                                     "is not yet initialized."};
    }

    if (library.get() != lib) {
        throw StitchSupportException{STITCH_SUPPORT_V1_ERROR_INVALID_LIB_HANDLE,
                                     "Cannot create a new collator when the Stitch Support Library "
                                     "is not yet initialized."};
    }

    auto statusWithCollator =
        CollatorFactoryInterface::get(lib->serviceContext.get())->makeFromBSON(collationSpecExpr);
    uassertStatusOK(statusWithCollator.getStatus());
    return new stitch_support_v1_collator(std::move(statusWithCollator.getValue()));
}

stitch_support_v1_matcher* matcher_create(stitch_support_v1_lib* const lib,
                                          BSONObj filter,
                                          stitch_support_v1_collator* collator) {
    if (!library) {
        throw StitchSupportException{STITCH_SUPPORT_V1_ERROR_LIBRARY_NOT_INITIALIZED,
                                     "Cannot create a new matcher when the Stitch Support Library "
                                     "is not yet initialized."};
    }

    if (library.get() != lib) {
        throw StitchSupportException{STITCH_SUPPORT_V1_ERROR_INVALID_LIB_HANDLE,
                                     "Cannot create a new matcher when the Stitch Support Library "
                                     "is not yet initialized."};
    }

    auto client = lib->serviceContext->makeClient("stitch_support");
    // TODO(SERVER-74662): Please revisit if this thread could be made killable.
    {
        stdx::lock_guard<mongo::Client> lk(*client.get());
        client.get()->setSystemOperationUnkillableByStepdown(lk);
    }

    return new stitch_support_v1_matcher(std::move(client), filter, collator);
}

stitch_support_v1_projection* projection_create(stitch_support_v1_lib* const lib,
                                                BSONObj spec,
                                                stitch_support_v1_matcher* matcher,
                                                stitch_support_v1_collator* collator) {
    if (!library) {
        throw StitchSupportException{STITCH_SUPPORT_V1_ERROR_LIBRARY_NOT_INITIALIZED,
                                     "Cannot create a new projection when the Stitch Support "
                                     "Library is not yet initialized."};
    }

    if (library.get() != lib) {
        throw StitchSupportException{STITCH_SUPPORT_V1_ERROR_INVALID_LIB_HANDLE,
                                     "Cannot create a new projection when the Stitch Support "
                                     "Library is not yet initialized."};
    }

    auto client = lib->serviceContext->makeClient("stitch_support");
    // TODO(SERVER-74662): Please revisit if this thread could be made killable.
    {
        stdx::lock_guard<mongo::Client> lk(*client.get());
        client.get()->setSystemOperationUnkillableByStepdown(lk);
    }

    return new stitch_support_v1_projection(std::move(client), spec, matcher, collator);
}

stitch_support_v1_update* update_create(stitch_support_v1_lib* const lib,
                                        BSONObj updateExpr,
                                        BSONArray arrayFilters,
                                        stitch_support_v1_matcher* matcher,
                                        stitch_support_v1_collator* collator) {
    if (!library) {
        throw StitchSupportException{
            STITCH_SUPPORT_V1_ERROR_LIBRARY_NOT_INITIALIZED,
            "Cannot create a new update when the Stitch Support Library is not yet initialized."};
    }

    if (library.get() != lib) {
        throw StitchSupportException{
            STITCH_SUPPORT_V1_ERROR_INVALID_LIB_HANDLE,
            "Cannot create a new udpate when the Stitch Support Library is not yet initialized."};
    }

    auto client = lib->serviceContext->makeClient("stitch_support");
    // TODO(SERVER-74662): Please revisit if this thread could be made killable.
    {
        stdx::lock_guard<mongo::Client> lk(*client.get());
        client.get()->setSystemOperationUnkillableByStepdown(lk);
    }

    return new stitch_support_v1_update(
        std::move(client), updateExpr, arrayFilters, matcher, collator);
}

int capi_status_get_error(const stitch_support_v1_status* const status) noexcept {
    invariant(status);
    return status->statusImpl.error;
}

const char* capi_status_get_what(const stitch_support_v1_status* const status) noexcept {
    invariant(status);
    return status->statusImpl.what.c_str();
}

int capi_status_get_code(const stitch_support_v1_status* const status) noexcept {
    invariant(status);
    return status->statusImpl.exception_code;
}

/**
 * toInterfaceType changes the compiler's interpretation from our internal BSON type 'char*' to
 * 'uint8_t*' which is the interface type of the Stitch library.
 */
auto toInterfaceType(char* bson) noexcept {
    return static_cast<uint8_t*>(static_cast<void*>(bson));
}

/**
 * fromInterfaceType changes the compiler's interpretation from 'uint8_t*' which is the BSON
 * interface type of the Stitch library to our internal type 'char*'.
 */
auto fromInterfaceType(const uint8_t* bson) noexcept {
    return static_cast<const char*>(static_cast<const void*>(bson));
}

}  // namespace
}  // namespace mongo

extern "C" {

stitch_support_v1_lib* MONGO_API_CALL stitch_support_v1_init(stitch_support_v1_status* status) {
    return enterCXX(mongo::getStatusImpl(status), [&]() { return mongo::stitch_lib_init(); });
}

int MONGO_API_CALL stitch_support_v1_fini(stitch_support_v1_lib* const lib,
                                          stitch_support_v1_status* const status) {
    return enterCXX(mongo::getStatusImpl(status), [&]() { return mongo::stitch_lib_fini(lib); });
}

int MONGO_API_CALL
stitch_support_v1_status_get_error(const stitch_support_v1_status* const status) {
    return mongo::capi_status_get_error(status);
}

const char* MONGO_API_CALL
stitch_support_v1_status_get_explanation(const stitch_support_v1_status* const status) {
    return mongo::capi_status_get_what(status);
}

int MONGO_API_CALL stitch_support_v1_status_get_code(const stitch_support_v1_status* const status) {
    return mongo::capi_status_get_code(status);
}

stitch_support_v1_status* MONGO_API_CALL stitch_support_v1_status_create(void) {
    return new stitch_support_v1_status;
}

void MONGO_API_CALL stitch_support_v1_status_destroy(stitch_support_v1_status* const status) {
    delete status;
}

stitch_support_v1_collator* MONGO_API_CALL
stitch_support_v1_collator_create(stitch_support_v1_lib* lib,
                                  const uint8_t* collationBSON,
                                  stitch_support_v1_status* const status) {
    return enterCXX(mongo::getStatusImpl(status), [&]() {
        mongo::BSONObj collationSpecExpr(mongo::fromInterfaceType(collationBSON));
        return mongo::collator_create(lib, collationSpecExpr);
    });
}

void MONGO_API_CALL stitch_support_v1_collator_destroy(stitch_support_v1_collator* const collator) {
    mongo::StitchSupportStatusImpl* nullStatus = nullptr;
    static_cast<void>(enterCXX(nullStatus, [=]() { delete collator; }));
}

stitch_support_v1_matcher* MONGO_API_CALL
stitch_support_v1_matcher_create(stitch_support_v1_lib* lib,
                                 const uint8_t* filterBSON,
                                 stitch_support_v1_collator* collator,
                                 stitch_support_v1_status* const status) {
    return enterCXX(mongo::getStatusImpl(status), [&]() {
        mongo::BSONObj filter(mongo::fromInterfaceType(filterBSON));
        return mongo::matcher_create(lib, filter, collator);
    });
}

void MONGO_API_CALL stitch_support_v1_matcher_destroy(stitch_support_v1_matcher* const matcher) {
    mongo::StitchSupportStatusImpl* nullStatus = nullptr;
    static_cast<void>(enterCXX(nullStatus, [=]() { delete matcher; }));
}

stitch_support_v1_projection* MONGO_API_CALL
stitch_support_v1_projection_create(stitch_support_v1_lib* lib,
                                    const uint8_t* specBSON,
                                    stitch_support_v1_matcher* matcher,
                                    stitch_support_v1_collator* collator,
                                    stitch_support_v1_status* const status) {
    return enterCXX(mongo::getStatusImpl(status), [&]() {
        mongo::BSONObj spec(mongo::fromInterfaceType(specBSON));
        return mongo::projection_create(lib, spec, matcher, collator);
    });
}

void MONGO_API_CALL
stitch_support_v1_projection_destroy(stitch_support_v1_projection* const projection) {
    mongo::StitchSupportStatusImpl* nullStatus = nullptr;
    static_cast<void>(enterCXX(nullStatus, [=]() { delete projection; }));
}

int MONGO_API_CALL stitch_support_v1_check_match(stitch_support_v1_matcher* matcher,
                                                 const uint8_t* documentBSON,
                                                 bool* isMatch,
                                                 stitch_support_v1_status* status) {
    return enterCXX(mongo::getStatusImpl(status), [&]() {
        mongo::BSONObj document(mongo::fromInterfaceType(documentBSON));
        *isMatch = matcher->matcher.matches(document, nullptr);
    });
}

uint8_t* MONGO_API_CALL
stitch_support_v1_projection_apply(stitch_support_v1_projection* const projection,
                                   const uint8_t* documentBSON,
                                   stitch_support_v1_status* status) {
    return enterCXX(mongo::getStatusImpl(status), [&]() {
        mongo::BSONObj document(mongo::fromInterfaceType(documentBSON));

        auto outputObj =
            projection->projectionExec->applyTransformation(mongo::Document{document}).toBson();
        auto outputSize = static_cast<size_t>(outputObj.objsize());
        auto output = new (std::nothrow) char[outputSize];

        uassert(mongo::ErrorCodes::ExceededMemoryLimit,
                "Failed to allocate memory for projection",
                output);

        static_cast<void>(std::copy_n(outputObj.objdata(), outputSize, output));
        return mongo::toInterfaceType(output);
    });
}

bool MONGO_API_CALL
stitch_support_v1_projection_requires_match(stitch_support_v1_projection* const projection) {
    return [projection]() noexcept {
        return projection->requiresMatch;
    }();
}

stitch_support_v1_update* MONGO_API_CALL
stitch_support_v1_update_create(stitch_support_v1_lib* lib,
                                const uint8_t* updateExprBSON,
                                const uint8_t* arrayFiltersBSON,
                                stitch_support_v1_matcher* matcher,
                                stitch_support_v1_collator* collator,
                                stitch_support_v1_status* status) {
    return enterCXX(mongo::getStatusImpl(status), [&]() {
        mongo::BSONObj updateExpr(mongo::fromInterfaceType(updateExprBSON));
        mongo::BSONArray arrayFilters(
            (arrayFiltersBSON ? mongo::BSONObj(mongo::fromInterfaceType(arrayFiltersBSON))
                              : mongo::BSONObj()));
        return mongo::update_create(lib, updateExpr, arrayFilters, matcher, collator);
    });
}

void MONGO_API_CALL stitch_support_v1_update_destroy(stitch_support_v1_update* const update) {
    mongo::StitchSupportStatusImpl* nullStatus = nullptr;
    static_cast<void>(enterCXX(nullStatus, [=]() { delete update; }));
}

uint8_t* MONGO_API_CALL
stitch_support_v1_update_apply(stitch_support_v1_update* const update,
                               const uint8_t* documentBSON,
                               stitch_support_v1_update_details* update_details,
                               stitch_support_v1_status* status) {
    return enterCXX(mongo::getStatusImpl(status), [&]() {
        mongo::BSONObj document(mongo::fromInterfaceType(documentBSON));
        std::string matchedField;

        if (update->updateDriver.needMatchDetails()) {
            invariant(update->matcher);

            mongo::MatchDetails matchDetails;
            matchDetails.requestElemMatchKey();
            bool isMatch = update->matcher->matcher.matches(document, &matchDetails);
            invariant(isMatch);
            if (matchDetails.hasElemMatchKey()) {
                matchedField = matchDetails.elemMatchKey();
            } else {
                // Empty 'matchedField' indicates that the matcher did not traverse an array.
            }
        }

        mongo::mutablebson::Document mutableDoc(document,
                                                mongo::mutablebson::Document::kInPlaceDisabled);

        mongo::FieldRefSet immutablePaths;  // Empty set
        bool docWasModified = false;

        mongo::FieldRefSetWithStorage modifiedPaths;

        uassertStatusOK(update->updateDriver.update(update->opCtx.get(),
                                                    matchedField,
                                                    &mutableDoc,
                                                    false /* validateForStorage */,
                                                    immutablePaths,
                                                    false /* isInsert */,
                                                    nullptr /* logOpRec*/,
                                                    &docWasModified,
                                                    &modifiedPaths));

        auto outputObj = mutableDoc.getObject();
        size_t outputSize = static_cast<size_t>(outputObj.objsize());
        auto output = new (std::nothrow) char[outputSize];

        uassert(
            mongo::ErrorCodes::ExceededMemoryLimit, "Failed to allocate memory for update", output);

        static_cast<void>(std::copy_n(outputObj.objdata(), outputSize, output));

        if (update_details) {
            update_details->modifiedPaths = modifiedPaths.serialize();
        }

        return mongo::toInterfaceType(output);
    });
}

uint8_t* MONGO_API_CALL stitch_support_v1_update_upsert(stitch_support_v1_update* const update,
                                                        stitch_support_v1_status* status) {
    return enterCXX(mongo::getStatusImpl(status), [=] {
        bool docWasModified = false;

        mongo::mutablebson::Document mutableDoc(mongo::BSONObj(),
                                                mongo::mutablebson::Document::kInPlaceDisabled);

        const mongo::FieldRef idFieldRef("_id");
        mongo::FieldRefSet immutablePaths;
        invariant(immutablePaths.insert(&idFieldRef));

        if (update->matcher) {
            uassertStatusOK(update->updateDriver.populateDocumentWithQueryFields(
                update->opCtx.get(),
                *update->matcher->matcher.getQuery(),
                immutablePaths,
                mutableDoc));
        }

        uassertStatusOK(update->updateDriver.update(update->opCtx.get(),
                                                    mongo::StringData() /* matchedField */,
                                                    &mutableDoc,
                                                    false /* validateForStorage */,
                                                    immutablePaths,
                                                    true /* isInsert */,
                                                    nullptr /* logOpRec */,
                                                    &docWasModified,
                                                    nullptr /* modifiedPaths */));

        auto outputObj = mutableDoc.getObject();
        size_t outputSize = static_cast<size_t>(outputObj.objsize());
        auto output = new (std::nothrow) char[outputSize];

        uassert(
            mongo::ErrorCodes::ExceededMemoryLimit, "Failed to allocate memory for upsert", output);

        static_cast<void>(std::copy_n(outputObj.objdata(), outputSize, output));
        return mongo::toInterfaceType(output);
    });
}

bool MONGO_API_CALL
stitch_support_v1_update_requires_match(stitch_support_v1_update* const update) {
    return [update]() {
        return update->updateDriver.needMatchDetails();
    }();
}

stitch_support_v1_update_details* MONGO_API_CALL stitch_support_v1_update_details_create(void) {
    return new stitch_support_v1_update_details;
};

void MONGO_API_CALL
stitch_support_v1_update_details_destroy(stitch_support_v1_update_details* update_details) {
    mongo::StitchSupportStatusImpl* nullStatus = nullptr;
    static_cast<void>(enterCXX(nullStatus, [=]() { delete update_details; }));
};

size_t MONGO_API_CALL stitch_support_v1_update_details_num_modified_paths(
    stitch_support_v1_update_details* update_details) {
    return update_details->modifiedPaths.size();
}

const char* MONGO_API_CALL stitch_support_v1_update_details_path(
    stitch_support_v1_update_details* update_details, size_t path_index) {
    invariant(path_index < update_details->modifiedPaths.size());
    return update_details->modifiedPaths[path_index].c_str();
}

void MONGO_API_CALL stitch_support_v1_bson_free(uint8_t* bson) {
    mongo::StitchSupportStatusImpl* nullStatus = nullptr;
    static_cast<void>(enterCXX(nullStatus, [=]() { delete[](bson); }));
}

}  // extern "C"
