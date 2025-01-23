/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/core/client/AWSError.h>
#include <aws/core/client/CoreErrors.h>
#include <aws/core/utils/memory/stl/AWSMap.h>
#include <aws/core/utils/HashingUtils.h>

using namespace Aws::Client;
using namespace Aws::Utils;
using namespace Aws::Http;

#ifdef _MSC_VER
#pragma warning(push)
// VS2015 compiler's bug, warning s_CoreErrorsMapper: symbol will be dynamically initialized (implementation limitation)
#pragma warning(disable : 4592)
#endif

using ErrorsMapperContainer = Aws::Map<Aws::String, AWSError<CoreErrors> >;
static ErrorsMapperContainer* s_CoreErrorsMapper(nullptr);

#ifdef _MSC_VER
#pragma warning(pop)
#endif

void CoreErrorsMapper::InitCoreErrorsMapper()
{
    if (s_CoreErrorsMapper)
    {
      return;
    }
    s_CoreErrorsMapper = Aws::New<ErrorsMapperContainer>("InitCoreErrorsMapper");

    s_CoreErrorsMapper->emplace("IncompleteSignature", AWSError<CoreErrors>(CoreErrors::INCOMPLETE_SIGNATURE, false));
    s_CoreErrorsMapper->emplace("IncompleteSignatureException", AWSError<CoreErrors>(CoreErrors::INCOMPLETE_SIGNATURE, false));
    s_CoreErrorsMapper->emplace("InvalidSignatureException", AWSError<CoreErrors>(CoreErrors::INVALID_SIGNATURE, false));
    s_CoreErrorsMapper->emplace("InvalidSignature", AWSError<CoreErrors>(CoreErrors::INVALID_SIGNATURE, false));
    s_CoreErrorsMapper->emplace("InternalFailureException", AWSError<CoreErrors>(CoreErrors::INTERNAL_FAILURE, true));
    s_CoreErrorsMapper->emplace("InternalFailure", AWSError<CoreErrors>(CoreErrors::INTERNAL_FAILURE, true));
    s_CoreErrorsMapper->emplace("InternalServerError", AWSError<CoreErrors>(CoreErrors::INTERNAL_FAILURE, true));
    s_CoreErrorsMapper->emplace("InternalError", AWSError<CoreErrors>(CoreErrors::INTERNAL_FAILURE, true));
    s_CoreErrorsMapper->emplace("InvalidActionException", AWSError<CoreErrors>(CoreErrors::INVALID_ACTION, false));
    s_CoreErrorsMapper->emplace("InvalidAction", AWSError<CoreErrors>(CoreErrors::INVALID_ACTION, false));
    s_CoreErrorsMapper->emplace("InvalidClientTokenIdException", AWSError<CoreErrors>(CoreErrors::INVALID_CLIENT_TOKEN_ID, false));
    s_CoreErrorsMapper->emplace("InvalidClientTokenId", AWSError<CoreErrors>(CoreErrors::INVALID_CLIENT_TOKEN_ID, false));
    s_CoreErrorsMapper->emplace("InvalidParameterCombinationException", AWSError<CoreErrors>(CoreErrors::INVALID_PARAMETER_COMBINATION, false));
    s_CoreErrorsMapper->emplace("InvalidParameterCombination", AWSError<CoreErrors>(CoreErrors::INVALID_PARAMETER_COMBINATION, false));
    s_CoreErrorsMapper->emplace("InvalidParameterValueException", AWSError<CoreErrors>(CoreErrors::INVALID_PARAMETER_VALUE, false));
    s_CoreErrorsMapper->emplace("InvalidParameterValue", AWSError<CoreErrors>(CoreErrors::INVALID_PARAMETER_VALUE, false));
    s_CoreErrorsMapper->emplace("InvalidQueryParameterException", AWSError<CoreErrors>(CoreErrors::INVALID_QUERY_PARAMETER, false));
    s_CoreErrorsMapper->emplace("InvalidQueryParameter", AWSError<CoreErrors>(CoreErrors::INVALID_QUERY_PARAMETER, false));
    s_CoreErrorsMapper->emplace("MalformedQueryStringException", AWSError<CoreErrors>(CoreErrors::MALFORMED_QUERY_STRING, false));
    s_CoreErrorsMapper->emplace("MalformedQueryString", AWSError<CoreErrors>(CoreErrors::MALFORMED_QUERY_STRING, false));
    s_CoreErrorsMapper->emplace("MissingActionException", AWSError<CoreErrors>(CoreErrors::MISSING_ACTION, false));
    s_CoreErrorsMapper->emplace("MissingAction", AWSError<CoreErrors>(CoreErrors::MISSING_ACTION, false));
    s_CoreErrorsMapper->emplace("MissingAuthenticationTokenException", AWSError<CoreErrors>(CoreErrors::MISSING_AUTHENTICATION_TOKEN, false));
    s_CoreErrorsMapper->emplace("MissingAuthenticationToken", AWSError<CoreErrors>(CoreErrors::MISSING_AUTHENTICATION_TOKEN, false));
    s_CoreErrorsMapper->emplace("MissingParameterException", AWSError<CoreErrors>(CoreErrors::MISSING_PARAMETER, false));
    s_CoreErrorsMapper->emplace("MissingParameter", AWSError<CoreErrors>(CoreErrors::MISSING_PARAMETER, false));
    s_CoreErrorsMapper->emplace("OptInRequired", AWSError<CoreErrors>(CoreErrors::OPT_IN_REQUIRED, false));
    s_CoreErrorsMapper->emplace("RequestExpiredException", AWSError<CoreErrors>(CoreErrors::REQUEST_EXPIRED, true));
    s_CoreErrorsMapper->emplace("RequestExpired", AWSError<CoreErrors>(CoreErrors::REQUEST_EXPIRED, true));
    s_CoreErrorsMapper->emplace("ServiceUnavailableException", AWSError<CoreErrors>(CoreErrors::SERVICE_UNAVAILABLE, true));
    s_CoreErrorsMapper->emplace("ServiceUnavailableError", AWSError<CoreErrors>(CoreErrors::SERVICE_UNAVAILABLE, true));
    s_CoreErrorsMapper->emplace("ServiceUnavailable", AWSError<CoreErrors>(CoreErrors::SERVICE_UNAVAILABLE, true));
    s_CoreErrorsMapper->emplace("RequestThrottledException", AWSError<CoreErrors>(CoreErrors::THROTTLING, true));
    s_CoreErrorsMapper->emplace("RequestThrottled", AWSError<CoreErrors>(CoreErrors::THROTTLING, true));
    s_CoreErrorsMapper->emplace("ThrottlingException", AWSError<CoreErrors>(CoreErrors::THROTTLING, true));
    s_CoreErrorsMapper->emplace("ThrottledException", AWSError<CoreErrors>(CoreErrors::THROTTLING, true));
    s_CoreErrorsMapper->emplace("Throttling", AWSError<CoreErrors>(CoreErrors::THROTTLING, true));
    s_CoreErrorsMapper->emplace("ValidationErrorException", AWSError<CoreErrors>(CoreErrors::VALIDATION, false));
    s_CoreErrorsMapper->emplace("ValidationException", AWSError<CoreErrors>(CoreErrors::VALIDATION, false));
    s_CoreErrorsMapper->emplace("ValidationError", AWSError<CoreErrors>(CoreErrors::VALIDATION, false));
    s_CoreErrorsMapper->emplace("AccessDeniedException", AWSError<CoreErrors>(CoreErrors::ACCESS_DENIED, false));
    s_CoreErrorsMapper->emplace("AccessDenied", AWSError<CoreErrors>(CoreErrors::ACCESS_DENIED, false));
    s_CoreErrorsMapper->emplace("ResourceNotFoundException", AWSError<CoreErrors>(CoreErrors::RESOURCE_NOT_FOUND, false));
    s_CoreErrorsMapper->emplace("ResourceNotFound", AWSError<CoreErrors>(CoreErrors::RESOURCE_NOT_FOUND, false));
    s_CoreErrorsMapper->emplace("UnrecognizedClientException", AWSError<CoreErrors>(CoreErrors::UNRECOGNIZED_CLIENT, false));
    s_CoreErrorsMapper->emplace("UnrecognizedClient", AWSError<CoreErrors>(CoreErrors::UNRECOGNIZED_CLIENT, false));
    s_CoreErrorsMapper->emplace("SlowDownException", AWSError<CoreErrors>(CoreErrors::SLOW_DOWN, true));
    s_CoreErrorsMapper->emplace("SlowDown", AWSError<CoreErrors>(CoreErrors::SLOW_DOWN, true));
    s_CoreErrorsMapper->emplace("SignatureDoesNotMatchException", AWSError<CoreErrors>(CoreErrors::SIGNATURE_DOES_NOT_MATCH, false));
    s_CoreErrorsMapper->emplace("SignatureDoesNotMatch", AWSError<CoreErrors>(CoreErrors::SIGNATURE_DOES_NOT_MATCH, false));
    s_CoreErrorsMapper->emplace("InvalidAccessKeyIdException", AWSError<CoreErrors>(CoreErrors::INVALID_ACCESS_KEY_ID, false));
    s_CoreErrorsMapper->emplace("InvalidAccessKeyId", AWSError<CoreErrors>(CoreErrors::INVALID_ACCESS_KEY_ID, false));
    s_CoreErrorsMapper->emplace("RequestTimeTooSkewedException", AWSError<CoreErrors>(CoreErrors::REQUEST_TIME_TOO_SKEWED, true));
    s_CoreErrorsMapper->emplace("RequestTimeTooSkewed", AWSError<CoreErrors>(CoreErrors::REQUEST_TIME_TOO_SKEWED, true));
    s_CoreErrorsMapper->emplace("RequestTimeoutException", AWSError<CoreErrors>(CoreErrors::REQUEST_TIMEOUT, true));
    s_CoreErrorsMapper->emplace("RequestTimeout", AWSError<CoreErrors>(CoreErrors::REQUEST_TIMEOUT, true));
}

void CoreErrorsMapper::CleanupCoreErrorsMapper()
{
    Aws::Delete(s_CoreErrorsMapper);
    s_CoreErrorsMapper = nullptr;
}

AWSError<CoreErrors> CoreErrorsMapper::GetErrorForName(const char* errorName)
{
    auto iter = s_CoreErrorsMapper->find(errorName);
    if (iter != s_CoreErrorsMapper->end())
    {
      return iter->second;
    }
    return AWSError<CoreErrors>(CoreErrors::UNKNOWN, false);
}

AWS_CORE_API AWSError<CoreErrors> CoreErrorsMapper::GetErrorForHttpResponseCode(HttpResponseCode code)
{
    // best effort attempt to map HTTP response codes to CoreErrors
    bool retryable = IsRetryableHttpResponseCode(code);
    AWSError<CoreErrors> error;
    switch (code)
    {
        case HttpResponseCode::UNAUTHORIZED:
        case HttpResponseCode::FORBIDDEN:
            error =  AWSError<CoreErrors>(CoreErrors::ACCESS_DENIED, retryable);
            break;
        case HttpResponseCode::NOT_FOUND:
            error = AWSError<CoreErrors>(CoreErrors::RESOURCE_NOT_FOUND, retryable);
            break;
        case HttpResponseCode::TOO_MANY_REQUESTS:
            error = AWSError<CoreErrors>(CoreErrors::SLOW_DOWN, retryable);
            break;
        case HttpResponseCode::INTERNAL_SERVER_ERROR:
            error = AWSError<CoreErrors>(CoreErrors::INTERNAL_FAILURE, retryable);
            break;
        case HttpResponseCode::BANDWIDTH_LIMIT_EXCEEDED:
            error = AWSError<CoreErrors>(CoreErrors::THROTTLING, retryable);
            break;
        case HttpResponseCode::SERVICE_UNAVAILABLE:
            error = AWSError<CoreErrors>(CoreErrors::SERVICE_UNAVAILABLE, retryable);
            break;
        case HttpResponseCode::REQUEST_TIMEOUT:
        case HttpResponseCode::AUTHENTICATION_TIMEOUT:
        case HttpResponseCode::LOGIN_TIMEOUT:
        case HttpResponseCode::GATEWAY_TIMEOUT:
        case HttpResponseCode::NETWORK_READ_TIMEOUT:
        case HttpResponseCode::NETWORK_CONNECT_TIMEOUT:
            error = AWSError<CoreErrors>(CoreErrors::REQUEST_TIMEOUT, retryable);
            break;
        default:
            int codeValue = static_cast<int>(code);
            error = AWSError<CoreErrors>(CoreErrors::UNKNOWN, codeValue >= 500 && codeValue < 600);
    }
    error.SetResponseCode(code);
    return error;
}

/**
 * Overload ostream operator<< for CoreErrors enum class for a prettier output such as "103" and not "<67-00 00-00>"
 */
Aws::OStream& Aws::Client::operator<< (Aws::OStream& oStream, CoreErrors code)
{
    oStream << Aws::Utils::StringUtils::to_string(static_cast<typename std::underlying_type<HttpResponseCode>::type>(code));
    return oStream;
}
