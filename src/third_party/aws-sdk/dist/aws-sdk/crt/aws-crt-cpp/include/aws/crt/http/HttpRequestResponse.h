#pragma once
/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/crt/Exports.h>
#include <aws/crt/Types.h>
#include <aws/crt/io/Stream.h>

struct aws_http_header;
struct aws_http_message;

namespace Aws
{
    namespace Crt
    {
        namespace Mqtt
        {
            class MqttConnection;
            class MqttConnectionCore;
        } // namespace Mqtt
        namespace Mqtt5
        {
            class Mqtt5ClientCore;
        }
        namespace Http
        {
            using HttpHeader = aws_http_header;

            /**
             * Base class representing a mutable http request or response.
             */
            class AWS_CRT_CPP_API HttpMessage
            {
              public:
                virtual ~HttpMessage();

                HttpMessage(const HttpMessage &) = delete;
                HttpMessage(HttpMessage &&) = delete;
                HttpMessage &operator=(const HttpMessage &) = delete;
                HttpMessage &operator=(HttpMessage &&) = delete;

                /**
                 * Gets the input stream representing the message body
                 */
                std::shared_ptr<Aws::Crt::Io::InputStream> GetBody() const noexcept;

                /**
                 * Sets the input stream representing the message body
                 * @param body the input stream representing the message body
                 * @return success/failure
                 */
                bool SetBody(const std::shared_ptr<Aws::Crt::Io::IStream> &body) noexcept;

                /**
                 * Sets the input stream representing the message body
                 * @param body the input stream representing the message body
                 * @return success/failure
                 */
                bool SetBody(const std::shared_ptr<Aws::Crt::Io::InputStream> &body) noexcept;

                /**
                 * Gets the number of headers contained in this request
                 * @return the number of headers contained in this request
                 */
                size_t GetHeaderCount() const noexcept;

                /**
                 * Gets a particular header in the request
                 * @param index index of the header to fetch
                 * @return an option containing the requested header if the index is in bounds
                 */
                Optional<HttpHeader> GetHeader(size_t index) const noexcept;

                /**
                 * Adds a header to the request
                 * @param header header to add
                 * @return success/failure
                 */
                bool AddHeader(const HttpHeader &header) noexcept;

                /**
                 * Removes a header from the request
                 * @param index index of the header to remove
                 * @return success/failure
                 */
                bool EraseHeader(size_t index) noexcept;

                /**
                 * @return true/false if the underlying object is valid
                 */
                operator bool() const noexcept { return m_message != nullptr; }

                /// @private
                struct aws_http_message *GetUnderlyingMessage() const noexcept { return m_message; }

              protected:
                HttpMessage(Allocator *allocator, struct aws_http_message *message) noexcept;

                Allocator *m_allocator;
                struct aws_http_message *m_message;
                std::shared_ptr<Aws::Crt::Io::InputStream> m_bodyStream;
            };

            /**
             * Class representing a mutable http request.
             */
            class AWS_CRT_CPP_API HttpRequest : public HttpMessage
            {
                friend class Mqtt::MqttConnectionCore;
                friend class Mqtt5::Mqtt5ClientCore;

              public:
                HttpRequest(Allocator *allocator = ApiAllocator());

                /**
                 * @return the value of the Http method associated with this request
                 */
                Optional<ByteCursor> GetMethod() const noexcept;

                /**
                 * Sets the value of the Http method associated with this request
                 */
                bool SetMethod(ByteCursor method) noexcept;

                /**
                 * @return the value of the URI-path associated with this request
                 */
                Optional<ByteCursor> GetPath() const noexcept;

                /**
                 * Sets the value of the URI-path associated with this request
                 */
                bool SetPath(ByteCursor path) noexcept;

              protected:
                HttpRequest(Allocator *allocator, struct aws_http_message *message);
            };

            /**
             * Class representing a mutable http response.
             */
            class AWS_CRT_CPP_API HttpResponse : public HttpMessage
            {
              public:
                HttpResponse(Allocator *allocator = ApiAllocator());

                /**
                 * @return the integral Http response code associated with this response
                 */
                Optional<int> GetResponseCode() const noexcept;

                /**
                 * Sets the integral Http response code associated with this response
                 */
                bool SetResponseCode(int response) noexcept;
            };
        } // namespace Http
    } // namespace Crt
} // namespace Aws
