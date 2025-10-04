/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/core/http/URI.h>

#include <aws/core/utils/memory/stl/AWSSet.h>
#include <aws/core/utils/logging/LogMacros.h>

#include <cstdlib>
#include <cctype>
#include <cassert>
#include <algorithm>
#include <iomanip>

using namespace Aws::Http;
using namespace Aws::Utils;

namespace Aws
{
namespace Http
{

const char* SEPARATOR = "://";

bool s_compliantRfc3986Encoding = false;
void SetCompliantRfc3986Encoding(bool compliant) { s_compliantRfc3986Encoding = compliant; }

bool s_preservePathSeparators = false;
void SetPreservePathSeparators(bool preservePathSeparators) { s_preservePathSeparators = preservePathSeparators; }

Aws::String urlEncodeSegment(const Aws::String& segment, bool rfcEncoded = false)
{
    // consolidates legacy escaping logic into one local method
    if (rfcEncoded || s_compliantRfc3986Encoding)
    {
        return StringUtils::URLEncode(segment.c_str());
    }
    else
    {
        Aws::StringStream ss;
        ss << std::hex << std::uppercase;
        for(unsigned char c : segment) // alnum results in UB if the value of c is not unsigned char & is not EOF
        {
            // RFC 3986 §2.3 unreserved characters
            if (StringUtils::IsAlnum(c))
            {
                ss << c;
                continue;
            }
            switch(c)
            {
                // §2.3 unreserved characters
                // The path section of the URL allows unreserved characters to appear unescaped
                case '-': case '_': case '.': case '~':
                // RFC 3986 §2.2 Reserved characters
                // NOTE: this implementation does not accurately implement the RFC on purpose to accommodate for
                // discrepancies in the implementations of URL encoding between AWS services for legacy reasons.
                case '$': case '&': case ',':
                case ':': case '=': case '@':
                    ss << c;
                    break;
                default:
                    ss << '%' << std::setfill('0') << std::setw(2) << (int)c << std::setw(0);
            }
        }
        return ss.str();
    }
}

} // namespace Http
} // namespace Aws

URI::URI() : m_scheme(Scheme::HTTP), m_port(HTTP_DEFAULT_PORT), m_pathHasTrailingSlash(false)
{
}

URI::URI(const Aws::String& uri) : m_scheme(Scheme::HTTP), m_port(HTTP_DEFAULT_PORT)
{
    ParseURIParts(uri);
}

URI::URI(const char* uri) : m_scheme(Scheme::HTTP), m_port(HTTP_DEFAULT_PORT)
{
    ParseURIParts(uri);
}

URI& URI::operator =(const Aws::String& uri)
{
    this->ParseURIParts(uri);
    return *this;
}

URI& URI::operator =(const char* uri)
{
    this->ParseURIParts(uri);
    return *this;
}

bool URI::operator ==(const URI& other) const
{
    return CompareURIParts(other);
}

bool URI::operator ==(const Aws::String& other) const
{
    return CompareURIParts(other);
}

bool URI::operator ==(const char* other) const
{
    return CompareURIParts(other);
}

bool URI::operator !=(const URI& other) const
{
    return !(*this == other);
}

bool URI::operator !=(const Aws::String& other) const
{
    return !(*this == other);
}

bool URI::operator !=(const char* other) const
{
    return !(*this == other);
}

void URI::SetScheme(Scheme value)
{
    assert(value == Scheme::HTTP || value == Scheme::HTTPS);

    if (value == Scheme::HTTP)
    {
        m_port = m_port == HTTPS_DEFAULT_PORT || m_port == 0 ? HTTP_DEFAULT_PORT : m_port;
        m_scheme = value;
    }
    else if (value == Scheme::HTTPS)
    {
        m_port = m_port == HTTP_DEFAULT_PORT || m_port == 0 ? HTTPS_DEFAULT_PORT : m_port;
        m_scheme = value;
    }
}

Aws::String URI::URLEncodePathRFC3986(const Aws::String& path, bool rfcCompliantEncoding)
{
    if (path.empty())
    {
        return path;
    }

    const Aws::Vector<Aws::String> pathParts = StringUtils::Split(path, '/');
    Aws::StringStream ss;
    ss << std::hex << std::uppercase;

    // escape characters appearing in a URL path according to RFC 3986
    for (const auto& segment : pathParts)
    {
        ss << '/' << urlEncodeSegment(segment, rfcCompliantEncoding);
    }

    // if the last character was also a slash, then add that back here.
    if (path.back() == '/')
    {
        ss << '/';
    }

    return ss.str();
}

Aws::String URI::URLEncodePath(const Aws::String& path)
{
    Aws::Vector<Aws::String> pathParts = StringUtils::Split(path, '/');
    Aws::StringStream ss;

    for (Aws::Vector<Aws::String>::iterator iter = pathParts.begin(); iter != pathParts.end(); ++iter)
    {
        ss << '/' << StringUtils::URLEncode(iter->c_str());
    }

    //if the last character was also a slash, then add that back here.
    if (path.length() > 0 && path[path.length() - 1] == '/')
    {
        ss << '/';
    }

    if (path.length() > 0 && path[0] != '/')
    {
        return ss.str().substr(1);
    }
    else
    {
        return ss.str();
    }
}

Aws::String URI::GetPath() const
{
    Aws::String path = "";

    for (auto const& segment : m_pathSegments)
    {
        path.push_back('/');
        path.append(segment);
    }

    if (m_pathSegments.empty() || m_pathHasTrailingSlash)
    {
        path.push_back('/');
    }

    return path;
}

Aws::String URI::GetURLEncodedPath() const
{
    Aws::StringStream ss;

    for (auto const& segment : m_pathSegments)
    {
        ss << '/' << StringUtils::URLEncode(segment.c_str());
    }

    if (m_pathSegments.empty() || m_pathHasTrailingSlash)
    {
        ss << '/';
    }

    return ss.str();
}

Aws::String URI::GetURLEncodedPathRFC3986() const
{
    Aws::StringStream ss;
    ss << std::hex << std::uppercase;

    // escape characters appearing in a URL path according to RFC 3986
    // (mostly; there is some non-standards legacy support that can be disabled)
    for (const auto& segment : m_pathSegments)
    {
        ss << '/' << urlEncodeSegment(segment, m_useRfcEncoding);
    }

    if (m_pathSegments.empty() || m_pathHasTrailingSlash)
    {
        ss << '/';
    }

    return ss.str();
}

void URI::SetPath(const Aws::String& value)
{
    m_pathSegments.clear();
    AddPathSegments(value);
}

//ugh, this isn't even part of the canonicalization spec. It is part of how our services have implemented their signers though....
//it doesn't really hurt anything to reorder it though, so go ahead and sort the values for parameters with the same key
void InsertValueOrderedParameter(QueryStringParameterCollection& queryParams, const Aws::String& key, const Aws::String& value)
{
    auto entriesAtKey = queryParams.equal_range(key);
    for (auto& entry = entriesAtKey.first; entry != entriesAtKey.second; ++entry)
    {
        if (entry->second > value)
        {
            queryParams.emplace_hint(entry, key, value);
            return;
        }
    }

    queryParams.emplace(key, value);
}

QueryStringParameterCollection URI::GetQueryStringParameters(bool decode) const
{
    Aws::String queryString = GetQueryString();

    QueryStringParameterCollection parameterCollection;

    //if we actually have a query string
    if (queryString.size() > 0)
    {
        size_t currentPos = 1, locationOfNextDelimiter = 1;

        //while we have params left to parse
        while (currentPos < queryString.size())
        {
            //find next key/value pair
            locationOfNextDelimiter = queryString.find('&', currentPos);

            Aws::String keyValuePair;

            //if this isn't the last parameter
            if (locationOfNextDelimiter != Aws::String::npos)
            {
                keyValuePair = queryString.substr(currentPos, locationOfNextDelimiter - currentPos);
            }
            //if it is the last parameter
            else
            {
                keyValuePair = queryString.substr(currentPos);
            }

            //split on =
            size_t locationOfEquals = keyValuePair.find('=');
            Aws::String key = keyValuePair.substr(0, locationOfEquals);
            Aws::String value = keyValuePair.substr(locationOfEquals + 1);

            if(decode)
            {
                InsertValueOrderedParameter(parameterCollection, StringUtils::URLDecode(key.c_str()), StringUtils::URLDecode(value.c_str()));
            }
            else
            {
                InsertValueOrderedParameter(parameterCollection, key, value);
            }

            currentPos += keyValuePair.size() + 1;
        }
    }

    return parameterCollection;
}

void URI::CanonicalizeQueryString()
{
    QueryStringParameterCollection sortedParameters = GetQueryStringParameters(false);
    Aws::StringStream queryStringStream;

    bool first = true;

    if(sortedParameters.size() > 0)
    {
        queryStringStream << "?";
    }

    if(m_queryString.find('=') != std::string::npos)
    {
        for (QueryStringParameterCollection::iterator iter = sortedParameters.begin();
             iter != sortedParameters.end(); ++iter)
        {
            if (!first)
            {
                queryStringStream << "&";
            }

            first = false;
            queryStringStream << iter->first.c_str() << "=" << iter->second.c_str();
        }

        m_queryString = queryStringStream.str();
    }
}

void URI::AddQueryStringParameter(const char* key, const Aws::String& value)
{
    if (m_queryString.size() <= 0)
    {
        m_queryString.append("?");
    }
    else
    {
        m_queryString.append("&");
    }

    m_queryString.append(StringUtils::URLEncode(key) + "=" + StringUtils::URLEncode(value.c_str()));
}

void URI::AddQueryStringParameter(const Aws::Map<Aws::String, Aws::String>& queryStringPairs)
{
    for(const auto& entry: queryStringPairs)
    {
        AddQueryStringParameter(entry.first.c_str(), entry.second);
    }
}

void URI::SetQueryString(const Aws::String& str)
{
    m_queryString = "";

    if (str.empty()) return;

    if (str.front() != '?')
    {
        m_queryString.append("?").append(str);
    }
    else
    {
       m_queryString = str;
    }
}

Aws::String URI::GetURIString(bool includeQueryString) const
{
    assert(m_authority.size() > 0);

    Aws::StringStream ss;
    ss << SchemeMapper::ToString(m_scheme) << SEPARATOR << m_authority;

    if (m_scheme == Scheme::HTTP && m_port != HTTP_DEFAULT_PORT)
    {
        ss << ":" << m_port;
    }
    else if (m_scheme == Scheme::HTTPS && m_port != HTTPS_DEFAULT_PORT)
    {
        ss << ":" << m_port;
    }

    if (!m_pathSegments.empty())
    {
        ss << GetURLEncodedPathRFC3986();
    }

    if(includeQueryString)
    {
        ss << m_queryString;
    }

    return ss.str();
}

void URI::ParseURIParts(const Aws::String& uri)
{
    ExtractAndSetScheme(uri);
    ExtractAndSetAuthority(uri);
    ExtractAndSetPort(uri);
    ExtractAndSetPath(uri);
    ExtractAndSetQueryString(uri);
}

void URI::ExtractAndSetScheme(const Aws::String& uri)
{
    size_t posOfSeparator = uri.find(SEPARATOR);

    if (posOfSeparator != Aws::String::npos)
    {
        Aws::String schemePortion = uri.substr(0, posOfSeparator);
        SetScheme(SchemeMapper::FromString(schemePortion.c_str()));
    }
    else
    {
        SetScheme(Scheme::HTTP);
    }
}

void URI::ExtractAndSetAuthority(const Aws::String& uri)
{
    size_t authorityStart = uri.find(SEPARATOR);

    if (authorityStart == Aws::String::npos)
    {
        authorityStart = 0;
    }
    else
    {
        authorityStart += 3;
    }

    size_t posEndOfAuthority=0;
    // are we extracting an ipv6 address?
    if (uri.length() > authorityStart && uri.at(authorityStart) == '[')
    {
        posEndOfAuthority = uri.find(']', authorityStart);
        if (posEndOfAuthority == Aws::String::npos) {
            AWS_LOGSTREAM_ERROR("Uri", "Malformed uri: " << uri.c_str());
        }
        else
        {
            ++posEndOfAuthority;
        }
    }
    else
    {
        size_t posOfEndOfAuthorityPort = uri.find(':', authorityStart);
        size_t posOfEndOfAuthoritySlash = uri.find('/', authorityStart);
        size_t posOfEndOfAuthorityQuery = uri.find('?', authorityStart);
        posEndOfAuthority = (std::min)({posOfEndOfAuthorityPort, posOfEndOfAuthoritySlash, posOfEndOfAuthorityQuery});
    }
    if (posEndOfAuthority == Aws::String::npos)
    {
        posEndOfAuthority = uri.length();
    }

    SetAuthority(uri.substr(authorityStart, posEndOfAuthority - authorityStart));
}

void URI::ExtractAndSetPort(const Aws::String& uri)
{
    size_t authorityStart = uri.find(SEPARATOR);

    if(authorityStart == Aws::String::npos)
    {
        authorityStart = 0;
    }
    else
    {
        authorityStart += 3;
    }

    size_t portSearchStart = authorityStart;
    // are we extracting an ipv6 address?
    if (uri.length() > portSearchStart && uri.at(portSearchStart) == '[')
    {
        size_t posEndOfAuthority = uri.find(']', portSearchStart);
        if (posEndOfAuthority == Aws::String::npos) {
            AWS_LOGSTREAM_ERROR("Uri", "Malformed uri: " << uri.c_str());
        }
        else
        {
            portSearchStart = posEndOfAuthority;
        }
    }

    size_t positionOfPortDelimiter = uri.find(':', portSearchStart);

    bool hasPort = positionOfPortDelimiter != Aws::String::npos;

    if ((uri.find('/', portSearchStart) < positionOfPortDelimiter) || (uri.find('?', portSearchStart) < positionOfPortDelimiter))
    {
        hasPort = false;
    }

    if (hasPort)
    {
        Aws::String strPort;

        size_t i = positionOfPortDelimiter + 1;
        char currentDigit = uri[i];

        while (std::isdigit(currentDigit))
        {
            strPort += currentDigit;
            currentDigit = uri[++i];
        }

        SetPort(static_cast<uint16_t>(atoi(strPort.c_str())));
    }
}

void URI::ExtractAndSetPath(const Aws::String& uri)
{
    size_t authorityStart = uri.find(SEPARATOR);

    if (authorityStart == Aws::String::npos)
    {
        authorityStart = 0;
    }
    else
    {
        authorityStart += 3;
    }

    size_t pathEnd = uri.find('?');

    if (pathEnd == Aws::String::npos)
    {
        pathEnd = uri.length();
    }

    Aws::String authorityAndPath = uri.substr(authorityStart, pathEnd - authorityStart);

    size_t pathStart = authorityAndPath.find('/');

    if (pathStart != Aws::String::npos)
    {
        SetPath(authorityAndPath.substr(pathStart, pathEnd - pathStart));
    }
    else
    {
        SetPath("/");
    }
}

void URI::ExtractAndSetQueryString(const Aws::String& uri)
{
    size_t queryStart = uri.find('?');

    if (queryStart != Aws::String::npos)
    {
        m_queryString = uri.substr(queryStart);
    }
}

Aws::String URI::GetFormParameters() const
{
    if(m_queryString.length() == 0)
    {
        return "";
    }
    else
    {
        return m_queryString.substr(1);
    }
}

bool URI::CompareURIParts(const URI& other) const
{
    return m_scheme == other.m_scheme && m_authority == other.m_authority && GetPath() == other.GetPath() && m_queryString == other.m_queryString;
}
