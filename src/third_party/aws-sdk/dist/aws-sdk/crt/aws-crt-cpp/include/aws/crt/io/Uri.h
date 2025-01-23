#pragma once
/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/crt/Types.h>

#include <aws/io/uri.h>

namespace Aws
{
    namespace Crt
    {
        namespace Io
        {
            /**
             * Contains a URI used for networking application protocols. This type is move-only.
             */
            class AWS_CRT_CPP_API Uri final
            {
              public:
                Uri() noexcept;
                ~Uri();

                /**
                 * Parses `cursor` as a URI. Upon failure the bool() operator will return false and LastError()
                 * will contain the errorCode.
                 */
                Uri(const ByteCursor &cursor, Allocator *allocator = ApiAllocator()) noexcept;

                /**
                 * Builds a URI from `builderOptions`. Upon failure the bool() operator will return false and
                 * LastError() will contain the errorCode.
                 */
                Uri(aws_uri_builder_options &builderOptions, Allocator *allocator = ApiAllocator()) noexcept;

                Uri(const Uri &);
                Uri &operator=(const Uri &);
                Uri(Uri &&uri) noexcept;
                Uri &operator=(Uri &&) noexcept;

                /**
                 * @return true if the instance is in a valid state, false otherwise.
                 */
                operator bool() const noexcept { return m_isInit; }

                /**
                 * @return the value of the last aws error encountered by operations on this instance.
                 */
                int LastError() const noexcept { return m_lastError; }

                /**
                 * @return the scheme portion of the URI if present (e.g. https, http, ftp etc....)
                 */
                ByteCursor GetScheme() const noexcept;

                /**
                 * @return the authority portion of the URI if present. This will contain host name and port if
                 * specified.
                 * */
                ByteCursor GetAuthority() const noexcept;

                /**
                 * @return the path portion of the URI. If no path was present, this will be set to '/'.
                 */
                ByteCursor GetPath() const noexcept;

                /**
                 * @return the query string portion of the URI if present.
                 */
                ByteCursor GetQueryString() const noexcept;

                /**
                 * @return the host name portion of the authority. (port will not be in this value).
                 */
                ByteCursor GetHostName() const noexcept;

                /**
                 * @return the port portion of the authority if a port was specified. If it was not, this will
                 * be set to 0. In that case, it is your responsibility to determine the correct port
                 * based on the protocol you're using.
                 */
                uint32_t GetPort() const noexcept;

                /** @return the Path and Query portion of the URI. In the case of Http, this likely the value for the
                 * URI parameter.
                 */
                ByteCursor GetPathAndQuery() const noexcept;

                /**
                 * @return The full URI as it was passed to or parsed from the constructors.
                 */
                ByteCursor GetFullUri() const noexcept;

              private:
                aws_uri m_uri;
                int m_lastError;
                bool m_isInit;
            };

            AWS_CRT_CPP_API Aws::Crt::String EncodeQueryParameterValue(ByteCursor paramValue);

        } // namespace Io
    } // namespace Crt
} // namespace Aws
