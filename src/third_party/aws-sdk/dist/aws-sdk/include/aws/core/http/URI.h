/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once

#include <aws/core/Core_EXPORTS.h>

#include <aws/core/http/Scheme.h>
#include <aws/core/utils/memory/stl/AWSMap.h>
#include <aws/core/utils/StringUtils.h>

#include <stdint.h>

namespace Aws
{
    namespace Http
    {
        extern AWS_CORE_API const char* SEPARATOR;
        static const uint16_t HTTP_DEFAULT_PORT = 80;
        static const uint16_t HTTPS_DEFAULT_PORT = 443;

        extern bool s_compliantRfc3986Encoding;
        AWS_CORE_API void SetCompliantRfc3986Encoding(bool compliant);

        extern AWS_CORE_API bool s_preservePathSeparators;
        AWS_CORE_API void SetPreservePathSeparators(bool preservePathSeparators);

        //per https://tools.ietf.org/html/rfc3986#section-3.4 there is nothing preventing servers from allowing
        //multiple values for the same key. So use a multimap instead of a map.
        typedef Aws::MultiMap<Aws::String, Aws::String> QueryStringParameterCollection;

        /**
         * class modeling universal resource identifier, but implemented for http
         */
        class AWS_CORE_API URI
        {
        public:
            /**
              * Defaults to http and port 80
              */
            URI();
            /**
              * Parses string and sets uri fields
              */
            URI(const Aws::String&);
            /**
              * Parses string and sets uri fields
              */
            URI(const char*);

            URI& operator = (const Aws::String&);
            URI& operator = (const char*);

            bool operator == (const URI&) const;
            bool operator == (const Aws::String&) const;
            bool operator == (const char*) const;
            bool operator != (const URI&) const;
            bool operator != (const Aws::String&) const;
            bool operator != (const char*) const;

            /**
            * scheme or protocol e.g. http, https, ftp
            */
            inline Scheme GetScheme() const { return m_scheme; }

            /**
            * Sets scheme, if the port is incompatible with this scheme, the port will automatically be set as well.
            */
            void SetScheme(Scheme value);

            /**
            * Gets the domain portion of the uri
            */
            inline const Aws::String& GetAuthority() const { return m_authority; }

            /**
            * Sets the domain portion of the uri
            */
            inline void SetAuthority(const Aws::String& value) { m_authority = value; }

            /**
            * Gets the port portion of the uri, defaults to 22 for ftp, 80 for http and 443 for https
            */
            inline uint16_t GetPort() const { return m_port; }

            /**
            * Sets the port portion of the uri, normally you will not have to do this. If the scheme is set to ftp, http
            * or https then the default ports will be set.
            */
            inline void SetPort(uint16_t value) { m_port = value; }

            /**
            * Gets the path portion of the uri e.g. the portion after the first slash after the authority and prior to the
            * query string. This is not url encoded.
            */
            Aws::String GetPath() const;

            /**
            * Gets the path portion of the uri, url encodes it and returns it
            */
            Aws::String GetURLEncodedPath() const;

            /**
             * Gets the path portion of the uri, url encodes it according to RFC3986 and returns it.
             */
            Aws::String GetURLEncodedPathRFC3986() const;

            /**
            * Sets the path portion of the uri. URL encodes it if needed
            */
            void SetPath(const Aws::String& value);

            /**
             * Add a path segment to the uri.
             * Leading slashes and trailing slashes will be removed.
             * Use AddPathSegments() to enable trailing slashes.
             */
            template<typename T>
            inline void AddPathSegment(T pathSegment)
            {
                Aws::StringStream ss;
                ss << pathSegment;
                Aws::String segment = ss.str();
                segment.erase(0, segment.find_first_not_of('/'));
                segment.erase(segment.find_last_not_of('/') + 1);
                m_pathSegments.push_back(segment);
                m_pathHasTrailingSlash = false;
            }

            /**
             * Add path segments to the uri.
             */
            template<typename T>
            inline void AddPathSegments(T pathSegments)
            {
                Aws::StringStream ss;
                ss << pathSegments;
                Aws::String segments = ss.str();
                const auto splitOption = s_preservePathSeparators
                                           ? Utils::StringUtils::SplitOptions::INCLUDE_EMPTY_SEGMENTS
                                           : Utils::StringUtils::SplitOptions::NOT_SET;
                // Preserve legacy behavior -- we need to remove a leading "/" if use `INCLUDE_EMPTY_SEGMENTS` is specified
                // because string split will no longer ignore leading delimiters -- which is correct.
                auto split = Aws::Utils::StringUtils::Split(segments, '/', splitOption);
                if (s_preservePathSeparators && m_pathSegments.empty() && !split.empty() && split.front().empty() && !m_pathHasTrailingSlash) {
                  split.erase(split.begin());
                }
                for (const auto& segment: split)
                {
                    m_pathSegments.push_back(segment);
                }
                m_pathHasTrailingSlash = (!segments.empty() && segments.back() == '/');
            }

            /**
            * Gets the raw query string including the ?
            */
            inline const Aws::String& GetQueryString() const { return m_queryString; }

            /**
             * Resets the query string to the raw string. all query string manipulations made before this call will be lost
             */
            void SetQueryString(const Aws::String& str);

            Aws::String GetFormParameters() const;

            /**
            * Canonicalizes the query string.
            */
            void CanonicalizeQueryString();

            /**
            * parses query string and returns url decoded key/value mappings from it. Spaces and all url encoded
            * values will not be encoded.
            */
            QueryStringParameterCollection GetQueryStringParameters(bool decode = true) const;

            /**
            * Adds query string parameter to underlying query string.
            */
            void AddQueryStringParameter(const char* key, const Aws::String& value);

            /**
            * Adds multiple query string parameters to underlying query string.
            */
            void AddQueryStringParameter(const Aws::Map<Aws::String, Aws::String>& queryStringPairs);

            /**
            * Converts the URI to a String usable for its context. e.g. an http request.
            */
            Aws::String GetURIString(bool includeQueryString = true) const;

            /**
            * Returns true if this URI is going to be encoded in Rfc3986 compliant mode
            */
            inline bool IsRfc3986Encoded() const { return m_useRfcEncoding; }

            /**
            * Sets Rfc3986 compliant encoding mode. False (i.e. use legacy encoding with some chars unescaped) is the default.
            */
            inline void SetRfc3986Encoded(const bool value) { m_useRfcEncoding = value; }

            /**
             * URLEncodes the path portions of path (doesn't encode the "/" portion)
             * Keeps the first and the last "/".
             */
            static Aws::String URLEncodePath(const Aws::String& path);

            /**
             * URLEncodes the path portion of the URI according to RFC3986
             */
            static Aws::String URLEncodePathRFC3986(const Aws::String& path, bool rfcCompliantEncoding = false);

        private:
            void ParseURIParts(const Aws::String& uri);
            void ExtractAndSetScheme(const Aws::String& uri);
            void ExtractAndSetAuthority(const Aws::String& uri);
            void ExtractAndSetPort(const Aws::String& uri);
            void ExtractAndSetPath(const Aws::String& uri);
            void ExtractAndSetQueryString(const Aws::String& uri);
            bool CompareURIParts(const URI& other) const;

            Scheme m_scheme = Scheme::HTTP;
            Aws::String m_authority;
            uint16_t m_port = HTTP_DEFAULT_PORT;
            Aws::Vector<Aws::String> m_pathSegments;
            bool m_pathHasTrailingSlash = false;
            bool m_useRfcEncoding = false;
            Aws::String m_queryString;
        };

    } // namespace Http
} // namespace Aws

