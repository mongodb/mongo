/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once

#include "aws/core/Core_EXPORTS.h"
#include "aws/core/utils/DateTime.h"

#include <memory>
#include <atomic>
#include <chrono>

namespace Aws
{
    namespace Http
    {
        class HttpRequest;
    } // namespace Http

    namespace Utils
    {
        namespace Event
        {
            class Message;
        }
    } // namespace Utils

    namespace Client
    {
        /**
         * Auth Signer interface. Takes a generic AWS request and applies crypto tamper resistant signatures on the request.
         */
        class AWS_CORE_API AWSAuthSigner
        {
        public:
            AWSAuthSigner() : m_clockSkew() { m_clockSkew.store(std::chrono::milliseconds(0L)); }
            virtual ~AWSAuthSigner() = default;

            /**
             * Signs the request itself (usually by adding a signature header) based on info in the request and uri.
             */
            virtual bool SignRequest(Aws::Http::HttpRequest& request) const = 0;

            /**
             * Signs the request itself (usually by adding a signature header) based on info in the request and uri.
             * If signBody is false and https is being used then the body of the payload will not be signed.
             * The default virtual function, just calls SignRequest.
             */
            virtual bool SignRequest(Aws::Http::HttpRequest& request, bool signBody) const
            {
                AWS_UNREFERENCED_PARAM(signBody);
                return SignRequest(request);
            }

            /**
             * Signs the request itself (usually by adding a signature header) based on info in the request and uri.
             * If signBody is false and https is being used then the body of the payload will not be signed.
             * The default virtual function, just calls SignRequest.
             * Using m_region by default if parameter region is nullptr.
             */
            virtual bool SignRequest(Aws::Http::HttpRequest& request, const char* region, bool signBody) const
            {
                AWS_UNREFERENCED_PARAM(signBody);
                AWS_UNREFERENCED_PARAM(region);
                return SignRequest(request);
            }

            /**
             * Signs the request itself (usually by adding a signature header) based on info in the request and uri.
             * If signBody is false and https is being used then the body of the payload will not be signed.
             * The default virtual function, just calls SignRequest.
             * Using m_region by default if parameter region is nullptr.
             * Using m_serviceName by default if parameter serviceName is nullptr.
             */
            virtual bool SignRequest(Aws::Http::HttpRequest& request, const char* region, const char* serviceName, bool signBody) const
            {
                AWS_UNREFERENCED_PARAM(signBody);
                AWS_UNREFERENCED_PARAM(region);
                AWS_UNREFERENCED_PARAM(serviceName);
                return SignRequest(request);
            }

            /**
             * Signs a single event message in an event stream.
             * The input message buffer is copied and signed. The message's input buffer will be deallocated and a new
             * buffer will be assigned. The new buffer encodes the original message with its headers as the payload of
             * the new message. The signature of the original message will be added as a header to the new message.
             *
             * A Hex encoded signature of the previous event (or of the HTTP request headers in case of the first event)
             * is provided as the 'priorSignature' parameter. 'priorSignature' will contain the value of the new
             * signature after this call returns successfully.
             *
             * The function returns true if the message is successfully signed.
             */
            virtual bool SignEventMessage(Aws::Utils::Event::Message&, Aws::String& /* priorSignature */) const { return false; }

            /**
             * Takes a request and signs the URI based on the HttpMethod, URI and other info from the request.
             * The URI can then be used in a normal HTTP call until expiration.
             */
            virtual bool PresignRequest(Aws::Http::HttpRequest& request, long long expirationInSeconds) const = 0;

            /**
            * Generates a signed Uri using the injected signer. for the supplied uri and http method and region. expirationInSeconds defaults
            * to 0 which is the default 7 days.
            * Using m_region by default if parameter region is nullptr.
            */
            virtual bool PresignRequest(Aws::Http::HttpRequest& request, const char* region, long long expirationInSeconds = 0) const = 0;

            /**
            * Generates a signed Uri using the injected signer. for the supplied uri and http method, region, and service name. expirationInSeconds defaults
            * to 0 which is the default 7 days.
            * Using m_region by default if parameter region is nullptr.
            * Using m_serviceName by default if parameter serviceName is nullptr.
            */
            virtual bool PresignRequest(Aws::Http::HttpRequest& request, const char* region, const char* serviceName, long long expirationInSeconds = 0) const = 0;

            /**
             * Return the signer's name
             */
            virtual const char* GetName() const = 0;

            /**
             * This handles detection of clock skew between clients and the server and adjusts the clock so that the next request will not
             * fail on the timestamp check.
             */
            virtual void SetClockSkew(const std::chrono::milliseconds& clockSkew) { m_clockSkew = clockSkew; }

            /**
             * Gets the timestamp being used by the signer. This may include a clock skew if a clock skew has been detected.
             */
            virtual Aws::Utils::DateTime GetSigningTimestamp() const { return Aws::Utils::DateTime::Now() + GetClockSkewOffset(); }

        protected:
            virtual std::chrono::milliseconds GetClockSkewOffset() const { return m_clockSkew.load(); }

            std::atomic<std::chrono::milliseconds> m_clockSkew;
        };
    } // namespace Client
} // namespace Aws
