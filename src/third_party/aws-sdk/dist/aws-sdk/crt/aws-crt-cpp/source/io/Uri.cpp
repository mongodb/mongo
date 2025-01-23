/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/crt/io/Uri.h>

namespace Aws
{
    namespace Crt
    {
        namespace Io
        {
            Uri::Uri() noexcept : m_lastError(AWS_ERROR_SUCCESS), m_isInit(false)
            {
                AWS_ZERO_STRUCT(m_uri);
            }

            Uri::~Uri()
            {
                if (m_isInit)
                {
                    aws_uri_clean_up(&m_uri);
                    m_isInit = false;
                }
            }

            Uri::Uri(const ByteCursor &cursor, Allocator *allocator) noexcept
                : m_lastError(AWS_ERROR_SUCCESS), m_isInit(false)
            {
                if (!aws_uri_init_parse(&m_uri, allocator, &cursor))
                {
                    m_isInit = true;
                }
                else
                {
                    m_lastError = aws_last_error();
                }
            }

            Uri::Uri(aws_uri_builder_options &builderOptions, Allocator *allocator) noexcept
                : m_lastError(AWS_ERROR_SUCCESS), m_isInit(false)
            {
                if (!aws_uri_init_from_builder_options(&m_uri, allocator, &builderOptions))
                {
                    m_isInit = true;
                }
                else
                {
                    m_lastError = aws_last_error();
                }
            }

            Uri::Uri(const Uri &other) : m_lastError(AWS_ERROR_SUCCESS), m_isInit(false)
            {
                if (other.m_isInit)
                {
                    ByteCursor uriCursor = other.GetFullUri();

                    if (!aws_uri_init_parse(&m_uri, other.m_uri.allocator, &uriCursor))
                    {
                        m_isInit = true;
                    }
                    else
                    {
                        m_lastError = aws_last_error();
                    }
                }
            }

            Uri &Uri::operator=(const Uri &other)
            {
                if (this != &other)
                {
                    m_isInit = false;
                    m_lastError = AWS_ERROR_SUCCESS;

                    if (other.m_isInit)
                    {
                        ByteCursor uriCursor = other.GetFullUri();

                        if (!aws_uri_init_parse(&m_uri, other.m_uri.allocator, &uriCursor))
                        {
                            m_isInit = true;
                        }
                        else
                        {
                            m_lastError = aws_last_error();
                        }
                    }
                }

                return *this;
            }

            Uri::Uri(Uri &&uri) noexcept : m_lastError(AWS_ERROR_SUCCESS), m_isInit(uri.m_isInit)
            {
                if (uri.m_isInit)
                {
                    m_uri = uri.m_uri;
                    AWS_ZERO_STRUCT(uri.m_uri);
                    uri.m_isInit = false;
                }
            }

            Uri &Uri::operator=(Uri &&uri) noexcept
            {
                if (this != &uri)
                {
                    if (m_isInit)
                    {
                        aws_uri_clean_up(&m_uri);
                    }

                    if (uri.m_isInit)
                    {
                        m_uri = uri.m_uri;
                        AWS_ZERO_STRUCT(uri.m_uri);
                        uri.m_isInit = false;
                        m_isInit = true;
                        m_lastError = AWS_ERROR_SUCCESS;
                    }
                    else
                    {
                        m_lastError = uri.m_lastError;
                    }
                }

                return *this;
            }

            ByteCursor Uri::GetScheme() const noexcept
            {
                return m_uri.scheme;
            }

            ByteCursor Uri::GetAuthority() const noexcept
            {
                return m_uri.authority;
            }

            ByteCursor Uri::GetPath() const noexcept
            {
                return m_uri.path;
            }

            ByteCursor Uri::GetQueryString() const noexcept
            {
                return m_uri.query_string;
            }

            ByteCursor Uri::GetHostName() const noexcept
            {
                return m_uri.host_name;
            }

            uint32_t Uri::GetPort() const noexcept
            {
                return m_uri.port;
            }

            ByteCursor Uri::GetPathAndQuery() const noexcept
            {
                return m_uri.path_and_query;
            }

            ByteCursor Uri::GetFullUri() const noexcept
            {
                return ByteCursorFromByteBuf(m_uri.uri_str);
            }

            Aws::Crt::String EncodeQueryParameterValue(ByteCursor paramValue)
            {
                struct aws_byte_buf encoded_buffer;
                aws_byte_buf_init(&encoded_buffer, ApiAllocator(), 3 * paramValue.len);

                auto encoding_result = aws_byte_buf_append_encoding_uri_param(&encoded_buffer, &paramValue);
                AWS_FATAL_ASSERT(AWS_OP_SUCCESS == encoding_result);

                auto encoded_value = Aws::Crt::String((const char *)encoded_buffer.buffer, encoded_buffer.len);
                aws_byte_buf_clean_up(&encoded_buffer);

                return encoded_value;
            }
        } // namespace Io
    } // namespace Crt
} // namespace Aws
