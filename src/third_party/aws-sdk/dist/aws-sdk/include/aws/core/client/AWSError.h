/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once

#include <aws/core/Core_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/core/http/HttpResponse.h>
#include <aws/core/utils/xml/XmlSerializer.h>
#include <aws/core/utils/json/JsonSerializer.h>
#include <aws/core/utils/StringUtils.h>

namespace Aws
{
    namespace Client
    {
        enum class CoreErrors;
        class XmlErrorMarshaller;
        class JsonErrorMarshaller;

        enum class ErrorPayloadType
        {
            NOT_SET,
            XML,
            JSON
        };

        enum class RetryableType
        {
          NOT_RETRYABLE,
          RETRYABLE,
          RETRYABLE_THROTTLING
        };

        /**
         * Container for Error enumerations with additional exception information. Name, message, retryable etc....
         */
        template<typename ERROR_TYPE>
        class AWSError
        {
        // Allow ErrorMarshaller to set error payload.
        friend class XmlErrorMarshaller;
        friend class JsonErrorMarshaller;
        friend class JsonErrorMarshallerQueryCompatible;
        template<typename T> friend class AWSError;
        public:
            /**
             * Initializes AWSError object as empty with the error not being retryable.
             */
            AWSError() :
              AWSError({}, RetryableType::NOT_RETRYABLE)
            {}

            /**
             * Initializes AWSError object with errorType, exceptionName, message, and retryable type.
             */
            AWSError(ERROR_TYPE errorType,
                     Aws::String exceptionName,
                     Aws::String message,
                     bool isRetryable)  :
              AWSError(errorType,
                isRetryable? RetryableType::RETRYABLE : RetryableType::NOT_RETRYABLE,
                exceptionName,
                message)
            {}

            /**
             * Initializes AWSError object with errorType and retryable flag. ExceptionName and message are empty.
             */
            AWSError(ERROR_TYPE errorType,
                     bool isRetryable) :
              AWSError(errorType,
                isRetryable? RetryableType::RETRYABLE : RetryableType::NOT_RETRYABLE)
            {}

            /**
             * Initializes AWSError object with errorType and retryable type. ExceptionName and message are empty.
             */
            AWSError(ERROR_TYPE errorType,
                     RetryableType retryableType,
                     Aws::String exceptionName = "",
                     Aws::String message = "") :
              m_errorType(errorType),
              m_exceptionName(std::move(exceptionName)),
              m_message(std::move(message)),
              m_retryableType(retryableType)
            {}

            AWSError(AWSError&&) = default;
            AWSError(const AWSError&) = default;

            template<typename OTHER_ERROR_TYPE>
            AWSError(AWSError<OTHER_ERROR_TYPE>&& rhs) :
                m_errorType(static_cast<ERROR_TYPE>(rhs.m_errorType)),
                m_exceptionName(std::move(rhs.m_exceptionName)),
                m_message(std::move(rhs.m_message)),
                m_remoteHostIpAddress(std::move(rhs.m_remoteHostIpAddress)),
                m_requestId(std::move(rhs.m_requestId)),
                m_responseHeaders(std::move(rhs.m_responseHeaders)),
                m_responseCode(rhs.m_responseCode),
                m_errorPayloadType(rhs.m_errorPayloadType),
                m_xmlPayload(std::move(rhs.m_xmlPayload)),
                m_jsonPayload(std::move(rhs.m_jsonPayload)),
                m_retryableType(rhs.m_retryableType)
            {}

            template<typename OTHER_ERROR_TYPE>
            AWSError(const AWSError<OTHER_ERROR_TYPE>& rhs) :
                m_errorType(static_cast<ERROR_TYPE>(rhs.m_errorType)),
                m_exceptionName(rhs.m_exceptionName),
                m_message(rhs.m_message),
                m_remoteHostIpAddress(rhs.m_remoteHostIpAddress),
                m_requestId(rhs.m_requestId),
                m_responseHeaders(rhs.m_responseHeaders),
                m_responseCode(rhs.m_responseCode),
                m_errorPayloadType(rhs.m_errorPayloadType),
                m_xmlPayload(rhs.m_xmlPayload),
                m_jsonPayload(rhs.m_jsonPayload),
                m_retryableType(rhs.m_retryableType)
            {}

            /**
             * Copy assignment operator
             */
            AWSError& operator=(const AWSError<ERROR_TYPE>&) = default;

            /**
             * Move assignment operator
             */
            AWSError& operator=(AWSError<ERROR_TYPE>&&) = default;

            /**
             * Gets underlying errorType.
             */
            inline const ERROR_TYPE GetErrorType() const { return m_errorType; }
            /**
             * Gets the underlying ExceptionName.
             */
            inline const Aws::String& GetExceptionName() const { return m_exceptionName; }
            /**
             *Sets the underlying ExceptionName.
             */
            inline void SetExceptionName(const Aws::String& exceptionName) { m_exceptionName = exceptionName; }
            /**
             * Gets the error message.
             */
#ifdef _WIN32
            #pragma push_macro("GetMessage")
#undef GetMessage
            inline const Aws::String& GetMessage() const { return m_message; }
            inline const Aws::String& GetMessageW() const { return GetMessage(); }
            inline const Aws::String& GetMessageA() const { return GetMessage(); }

#pragma pop_macro("GetMessage")
#else
            inline const Aws::String& GetMessage() const { return m_message; }
#endif //#ifdef _WIN32
            /**
             * Sets the error message
             */
            inline void SetMessage(const Aws::String& message) { m_message = message; }
            /**
             * Gets the resolved remote host IP address.
             * This value is only available after DNS resolution and with CURL http client right now.
             * Otherwise an empty string is returned.
             */
            inline const Aws::String& GetRemoteHostIpAddress() const { return m_remoteHostIpAddress; }
            /**
             * Sets the resolved remote host IP address.
             */
            inline void SetRemoteHostIpAddress(const Aws::String& remoteHostIpAddress) { m_remoteHostIpAddress = remoteHostIpAddress; }
            /**
             * Gets the request ID.
             * This value is available after request is made and when services return it in response.
             * Otherwise an empty string is returned.
             */
            inline const Aws::String& GetRequestId() const { return m_requestId; }
            /**
             * Sets the request ID.
             */
            inline void SetRequestId(const Aws::String& requestId) { m_requestId = requestId; }
            /**
             * Returns whether or not this error is eligible for retry.
             */
            inline bool ShouldRetry() const { return m_retryableType == RetryableType::RETRYABLE || m_retryableType == RetryableType::RETRYABLE_THROTTLING; }
            /**
             * Gets the response headers from the http response.
             */
            inline const Aws::Http::HeaderValueCollection& GetResponseHeaders() const { return m_responseHeaders; }
            /**
             * Sets the response headers from the http response.
             */
            inline void SetResponseHeaders(const Aws::Http::HeaderValueCollection& headers) { m_responseHeaders = headers; }
            /**
             * Tests whether or not a header exists.
             */
            inline bool ResponseHeaderExists(const Aws::String& headerName) const { return m_responseHeaders.find(Aws::Utils::StringUtils::ToLower(headerName.c_str())) != m_responseHeaders.end(); }
            /**
             * Gets the response code from the http response
             */
            inline Aws::Http::HttpResponseCode GetResponseCode() const { return m_responseCode; }
            /**
             * Sets the response code from the http response
             */
            inline void SetResponseCode(Aws::Http::HttpResponseCode responseCode) { m_responseCode = responseCode; }
            /**
             * Return whether or not the error should throttle retry strategies.
             */
            inline bool ShouldThrottle() const { return m_retryableType == RetryableType::RETRYABLE_THROTTLING; }
            /**
             * Sets the response code from the http response
             */
            inline void SetRetryableType(const RetryableType retryableType) { m_retryableType = retryableType; }

        protected:
            inline ErrorPayloadType GetErrorPayloadType() { return m_errorPayloadType; }
            inline void SetXmlPayload(const Aws::Utils::Xml::XmlDocument& xmlPayload)
            {
                m_errorPayloadType = ErrorPayloadType::XML;
                m_xmlPayload = xmlPayload;
            }
            inline void SetXmlPayload(Aws::Utils::Xml::XmlDocument&& xmlPayload)
            {
                m_errorPayloadType = ErrorPayloadType::XML;
                m_xmlPayload = std::move(xmlPayload);
            }
            inline const Aws::Utils::Xml::XmlDocument& GetXmlPayload() const
            {
                assert(m_errorPayloadType != ErrorPayloadType::JSON);
                return m_xmlPayload;
            }
            inline void SetJsonPayload(const Aws::Utils::Json::JsonValue& jsonPayload)
            {
                m_errorPayloadType = ErrorPayloadType::JSON;
                m_jsonPayload = jsonPayload;
            }
            inline void SetJsonPayload(Aws::Utils::Json::JsonValue&& jsonPayload)
            {
                m_errorPayloadType = ErrorPayloadType::JSON;
                m_jsonPayload = std::move(jsonPayload);
            }
            inline const Aws::Utils::Json::JsonValue& GetJsonPayload() const
            {
                assert(m_errorPayloadType != ErrorPayloadType::XML);
                return m_jsonPayload;
            }

            ERROR_TYPE m_errorType;
            Aws::String m_exceptionName;
            Aws::String m_message;
            Aws::String m_remoteHostIpAddress;
            Aws::String m_requestId;
            Aws::Http::HeaderValueCollection m_responseHeaders;
            Aws::Http::HttpResponseCode m_responseCode = Aws::Http::HttpResponseCode::REQUEST_NOT_MADE;

            ErrorPayloadType m_errorPayloadType = ErrorPayloadType::NOT_SET;
            Aws::Utils::Xml::XmlDocument m_xmlPayload;
            Aws::Utils::Json::JsonValue m_jsonPayload;
            RetryableType m_retryableType;
        };

        template<typename T>
        Aws::OStream& operator << (Aws::OStream& s, const AWSError<T>& e)
        {
            s << "HTTP response code: " << static_cast<int>(e.GetResponseCode()) << "\n"
              << "Resolved remote host IP address: " << e.GetRemoteHostIpAddress() << "\n"
              << "Request ID: " << e.GetRequestId() << "\n"
              << "Exception name: " << e.GetExceptionName() << "\n"
              << "Error message: " << e.GetMessage() << "\n"
              << e.GetResponseHeaders().size() << " response headers:";

            for (auto&& header : e.GetResponseHeaders())
            {
                s << "\n" << header.first << " : " << header.second;
            }
            return s;
        }

    } // namespace Client
} // namespace Aws
