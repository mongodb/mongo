/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/core/client/AWSError.h>
#include <aws/s3/S3ErrorMarshaller.h>
#include <aws/s3/S3Errors.h>
#include <aws/core/utils/xml/XmlSerializer.h>
#include <aws/core/http/HttpResponse.h>
#include <aws/core/utils/logging/LogMacros.h>

using namespace Aws::Client;
using namespace Aws::S3;
using namespace Aws::Utils::Xml;
using namespace Aws::Http;

AWSError<CoreErrors> S3ErrorMarshaller::FindErrorByName(const char* errorName) const
{
  AWSError<CoreErrors> error = S3ErrorMapper::GetErrorForName(errorName);
  if(error.GetErrorType() != CoreErrors::UNKNOWN)
  {
    return error;
  }

  return AWSErrorMarshaller::FindErrorByName(errorName);
}

Aws::String S3ErrorMarshaller::ExtractRegion(const AWSError<CoreErrors>& error) const
{
  const auto& headers = error.GetResponseHeaders();
  const auto& iter = headers.find("x-amz-bucket-region");
  if (iter != headers.end())
  {
    return iter->second;
  }

  const Aws::Utils::Xml::XmlDocument& xmlDocument = GetXmlPayloadFromError(error);
  Aws::Utils::Xml::XmlNode xmlNode = xmlDocument.GetRootElement();
  if (!xmlNode.IsNull())
  {
    Aws::Utils::Xml::XmlNode regionNode = xmlNode.FirstChild("Region");
    if (!regionNode.IsNull())
    {
      return regionNode.GetText().c_str();
    }
  }

  // as a last choice, try finding region from endpoint.
  const auto& locIter = headers.find("location");
  if (locIter != headers.end())
  {
    Aws::Http::URI uri(locIter->second);
    auto authority = uri.GetAuthority();
    // virtual address example: <bucketname>.<[s3-]region>.amazonaws.com
    // path style example: <[s3]-region>.amazonaws.com/<bucketname>
    auto pos = authority.find(".amazonaws.com");
    if (pos == 0 || pos == std::string::npos)
    {
      return {};
    }
    auto endPos = pos - 1;
    while (pos > 0 && authority[pos - 1] != '.')
    {
      pos--;
    }
    auto region = authority.substr(pos, endPos + 1 - pos);
    if (region.compare(0, 3, "s3-") == 0)
    {
        region = region.substr(3);
    }
    if (region.compare(0, 5, "fips-") == 0)
    {
        region = region.substr(5);
    }
    return region;
  }
  return {};
}

Aws::String S3ErrorMarshaller::ExtractEndpoint(const AWSError<CoreErrors>& error) const
{
  const auto& headers = error.GetResponseHeaders();
  const auto& iter = headers.find("location");
  if (iter != headers.end())
  {
    Aws::Http::URI uri(iter->second);
    return uri.GetAuthority();
  }

  const Aws::Utils::Xml::XmlDocument& xmlDocument = GetXmlPayloadFromError(error);
  Aws::Utils::Xml::XmlNode xmlNode = xmlDocument.GetRootElement();
  if (!xmlNode.IsNull())
  {
    Aws::Utils::Xml::XmlNode endpointNode = xmlNode.FirstChild("Endpoint");
    if (!endpointNode.IsNull())
    {
      Aws::Http::URI uri(endpointNode.GetText().c_str());
      return uri.GetAuthority();
    }
  }

  return {};
}
AWSError<Aws::Client::CoreErrors> S3ErrorMarshaller::Marshall(const Aws::Http::HttpResponse& httpResponse) const
{
  //if response code not ok, use error marshaller
  if(httpResponse.GetResponseCode() != HttpResponseCode::OK)
  {
    return XmlErrorMarshaller::Marshall(httpResponse);
  }

  Aws::String message{"Error in body of the response"};
  //extract error message and code in the body
  auto& body = httpResponse.GetResponseBody();
  
  if(!body.good())
  {
      return  Aws::Client::AWSError<Aws::Client::CoreErrors>(
                            Aws::Client::CoreErrors::INVALID_PARAMETER_VALUE,
                            "",
                            message,
                            false);
  }

  auto readPointer = body.tellg();
  XmlDocument doc = XmlDocument::CreateFromXmlStream(body);
  body.seekg(readPointer);
  Aws::String bodyError;
  Aws::String requestId;

  if (doc.WasParseSuccessful() &&
      !doc.GetRootElement().IsNull() && 
      doc.GetRootElement().GetName() == Aws::String("Error")) 
  {        
      //check if the first node fetched has no children
      auto messageNode = doc.GetRootElement().FirstChild("Message") ;
      if(!messageNode.IsNull())
      {
          message = messageNode.GetText();
      }
      auto codeNode = doc.GetRootElement().FirstChild("Code") ;
      if(!codeNode.IsNull())
      {
          bodyError = codeNode.GetText();
      }

      auto requestIdNode = doc.GetRootElement().FirstChild("RequestId") ;
      if(!requestIdNode.IsNull())
      {
        requestId = requestIdNode.GetText();
      }
  }

  auto error = FindErrorByName(bodyError.c_str());

  error.SetMessage(message);
  error.SetRequestId(requestId);

  return error;

}