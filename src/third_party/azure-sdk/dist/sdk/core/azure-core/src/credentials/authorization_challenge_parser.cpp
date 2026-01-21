// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "azure/core/internal/credentials/authorization_challenge_parser.hpp"

#include "azure/core/internal/strings.hpp"

#include <set>

using Azure::Core::Credentials::_detail::AuthorizationChallengeHelper;
using Azure::Core::Credentials::_internal::AuthorizationChallengeParser;

using Azure::Core::_internal::StringExtensions;
using Azure::Core::Http::HttpStatusCode;
using Azure::Core::Http::RawResponse;

// The parser is implemented to closely mimic the logic in .NET SDK:
// https://github.com/Azure/azure-sdk-for-net/blob/b7efe81fe69e020df74853b0f501ca6ccd5c94a1/sdk/core/Azure.Core/src/Shared/AuthorizationChallengeParser.cs
namespace {
class StringSpan final {
  std::string const* m_stringPtr;
  int m_startPos;
  int m_endPosExclusive;

public:
  StringSpan(std::string const* stringPtr = nullptr);
  StringSpan(StringSpan const&) = default;
  StringSpan& operator=(StringSpan const&) = default;

  int Length() const;
  std::string ToString() const;
  StringSpan Slice(int start) const;
  StringSpan Slice(int start, int length) const;
  StringSpan TrimStart(std::set<char> const& chars) const;
  StringSpan Trim(std::set<char> const& chars) const;
  int IndexOfAny(std::set<char> const& chars) const;
  bool CaseInsensitiveEquals(StringSpan const& other) const;
};

bool TryGetNextChallenge(StringSpan& headerValue, StringSpan& challengeKey);
bool TryGetNextParameter(StringSpan& headerValue, StringSpan& paramKey, StringSpan& paramValue);

std::string const EmptyString;
} // namespace

std::string const& AuthorizationChallengeHelper::GetChallenge(RawResponse const& response)
{
  // See RFC7235 (https://www.rfc-editor.org/rfc/rfc7235#section-4.1)
  if (response.GetStatusCode() == HttpStatusCode::Unauthorized)
  {
    auto const& headers = response.GetHeaders();
    auto const wwwAuthHeader = headers.find("WWW-Authenticate");
    if (wwwAuthHeader != headers.end())
    {
      return wwwAuthHeader->second;
    }
  }

  return EmptyString;
}

std::string AuthorizationChallengeParser::GetChallengeParameter(
    std::string const& challenge,
    std::string const& challengeScheme,
    std::string const& challengeParameter)
{
  StringSpan bearer = &challengeScheme;
  StringSpan claims = &challengeParameter;
  StringSpan headerSpan = &challenge;

  // Iterate through each challenge value.
  StringSpan challengeKey;
  while (TryGetNextChallenge(headerSpan, challengeKey))
  {
    // Enumerate each key=value parameter until we find the 'claims' key on the 'Bearer'
    // challenge.
    StringSpan key;
    StringSpan value;
    while (TryGetNextParameter(headerSpan, key, value))
    {
      if (challengeKey.CaseInsensitiveEquals(bearer) && key.CaseInsensitiveEquals(claims))
      {
        return value.ToString();
      }
    }
  }

  return {};
}

namespace {
std::set<char> const Space = {' '};
std::set<char> const SpaceOrComma = {' ', ','};
std::set<char> const Separator = {'='};
std::set<char> const Quote = {'\"'};

bool TryGetNextChallenge(StringSpan& headerValue, StringSpan& challengeKey)
{
  challengeKey = StringSpan();

  headerValue = headerValue.TrimStart(Space);
  int endOfChallengeKey = headerValue.IndexOfAny(Space);

  if (endOfChallengeKey < 0)
  {
    return false;
  }

  challengeKey = headerValue.Slice(0, endOfChallengeKey);

  // Slice the challenge key from the headerValue
  headerValue = headerValue.Slice(endOfChallengeKey + 1);

  return true;
}

bool TryGetNextParameter(StringSpan& headerValue, StringSpan& paramKey, StringSpan& paramValue)
{
  paramKey = StringSpan();
  paramValue = StringSpan();

  // Trim any separator prefixes.
  headerValue = headerValue.TrimStart(SpaceOrComma);

  int nextSpace = headerValue.IndexOfAny(Space);
  int nextSeparator = headerValue.IndexOfAny(Separator);

  if (nextSpace < nextSeparator && nextSpace != -1)
  {
    // we encountered another challenge value.
    return false;
  }

  if (nextSeparator < 0)
  {
    return false;
  }

  // Get the paramKey.
  paramKey = headerValue.Slice(0, nextSeparator).Trim(Space);

  // Slice to remove the 'paramKey=' from the parameters.
  headerValue = headerValue.Slice(nextSeparator + 1);

  // The start of paramValue will usually be a quoted string. Find the first quote.
  int quoteIndex = headerValue.IndexOfAny(Quote);

  // Get the paramValue, which is delimited by the trailing quote.
  headerValue = headerValue.Slice(quoteIndex + 1);
  if (quoteIndex >= 0)
  {
    // The values are quote wrapped
    paramValue = headerValue.Slice(0, headerValue.IndexOfAny(Quote));
  }
  else
  {
    // the values are not quote wrapped (storage is one example of this)
    //  either find the next space indicating the delimiter to the next value, or go to the end
    //  since this is the last value.
    int trailingDelimiterIndex = headerValue.IndexOfAny(SpaceOrComma);
    if (trailingDelimiterIndex >= 0)
    {
      paramValue = headerValue.Slice(0, trailingDelimiterIndex);
    }
    else
    {
      paramValue = headerValue;
    }
  }

  // Slice to remove the '"paramValue"' from the parameters.
  if (!headerValue.CaseInsensitiveEquals(paramValue))
    headerValue = headerValue.Slice(paramValue.Length() + 1);

  return true;
}

StringSpan::StringSpan(std::string const* stringPtr)
    : m_stringPtr(stringPtr), m_startPos(0),
      m_endPosExclusive(stringPtr ? static_cast<int>(stringPtr->size()) : 0)
{
}

int StringSpan::Length() const { return m_endPosExclusive - m_startPos; }

std::string StringSpan::ToString() const
{
  return m_stringPtr
      ? m_stringPtr->substr(static_cast<size_t>(m_startPos), static_cast<size_t>(Length()))
      : std::string{};
}

StringSpan StringSpan::Slice(int start) const
{
  StringSpan result = *this;
  result.m_startPos += start;
  return result;
}

StringSpan StringSpan::Slice(int start, int length) const
{
  auto result = Slice(start);
  result.m_endPosExclusive = result.m_startPos + length;
  return result;
}

StringSpan StringSpan::TrimStart(std::set<char> const& chars) const
{
  StringSpan result = *this;

  auto pos = result.m_startPos;
  for (; pos < result.m_endPosExclusive; ++pos)
  {
    if (chars.find(result.m_stringPtr->operator[](static_cast<size_t>(pos))) == chars.end())
    {
      break;
    }
  }

  result.m_startPos = pos;
  return result;
}

StringSpan StringSpan::Trim(std::set<char> const& chars) const
{
  StringSpan result = TrimStart(chars);

  auto endPos = m_endPosExclusive;
  for (; endPos > result.m_startPos; --endPos)
  {
    if (chars.find(result.m_stringPtr->operator[](static_cast<size_t>(endPos - 1))) == chars.end())
    {
      break;
    }
  }

  result.m_endPosExclusive = endPos;
  return result;
}

int StringSpan::IndexOfAny(std::set<char> const& chars) const
{
  for (auto pos = m_startPos; pos < m_endPosExclusive; ++pos)
  {
    if (chars.find(m_stringPtr->operator[](static_cast<size_t>(pos))) != chars.end())
    {
      return pos - m_startPos;
    }
  }

  return -1;
}

bool StringSpan::CaseInsensitiveEquals(StringSpan const& other) const
{
  auto const length = Length();
  if (length != other.Length())
  {
    return false;
  }

  for (auto offset = 0; offset < length; ++offset)
  {
    if (StringExtensions::ToLower(m_stringPtr->operator[](static_cast<size_t>(m_startPos + offset)))
        != StringExtensions::ToLower(
            other.m_stringPtr->operator[](static_cast<size_t>(other.m_startPos + offset))))
    {
      return false;
    }
  }

  return true;
}
} // namespace
