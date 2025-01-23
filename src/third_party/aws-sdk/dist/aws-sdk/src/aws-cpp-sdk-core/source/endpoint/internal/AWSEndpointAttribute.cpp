/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/core/endpoint/internal/AWSEndpointAttribute.h>
#include <aws/core/utils/logging/LogMacros.h>

static const char ENDPOINT_AUTH_SCHEME_TAG[] = "EndpointAuthScheme::BuildEndpointAuthSchemeFromJson";

Aws::String CrtToSdkSignerName(const Aws::String& crtSignerName)
{
    Aws::String sdkSigner = "NullSigner";
    if (crtSignerName == "sigv4") {
        sdkSigner = "SignatureV4";
    } else if (crtSignerName == "sigv4a") {
        sdkSigner = "AsymmetricSignatureV4";
    } else if (crtSignerName == "none") {
        sdkSigner = "NullSigner";
    } else if (crtSignerName == "bearer") {
        sdkSigner = "Bearer";
    } else if (crtSignerName == "sigv4-s3express") {
        sdkSigner = "S3ExpressSigner";
    } else {
        AWS_LOG_WARN(ENDPOINT_AUTH_SCHEME_TAG, (Aws::String("Unknown Endpoint authSchemes signer: ") + crtSignerName).c_str());
    }

    return sdkSigner;
}

size_t GetAuthSchemePriority(const Aws::String& authSchemeName)
{
    if(authSchemeName == "NullSigner" || authSchemeName.empty())
        return 0;
    if(authSchemeName == "SignatureV4")
        return 1;
    if(authSchemeName == "AsymmetricSignatureV4")
        return 2;
    if(authSchemeName == "Bearer")
        return 2;
    if (authSchemeName == "S3ExpressSigner")
        return 3;

    return 0; // unknown thus unsupported
}


Aws::Internal::Endpoint::EndpointAttributes
Aws::Internal::Endpoint::EndpointAttributes::BuildEndpointAttributesFromJson(const Aws::String& iJsonStr)
{
    Aws::Internal::Endpoint::EndpointAttributes attributes;
    Aws::Internal::Endpoint::EndpointAuthScheme& authScheme = attributes.authScheme;
    attributes.useS3ExpressAuth = false;

    Utils::Json::JsonValue jsonObject(iJsonStr);
    if (jsonObject.WasParseSuccessful())
    {
        Aws::Map<Aws::String, Utils::Json::JsonView> jsonMap = jsonObject.View().GetAllObjects();
        for (const auto& mapItemAttribute : jsonMap)
        {
            if (mapItemAttribute.first == "authSchemes" && mapItemAttribute.second.IsListType()) {
                Aws::Utils::Array<Utils::Json::JsonView> jsonAuthSchemeArray = mapItemAttribute.second.AsArray();

                for (size_t arrayIdx = 0; arrayIdx < jsonAuthSchemeArray.GetLength(); ++arrayIdx)
                {
                    const Utils::Json::JsonView& property = jsonAuthSchemeArray.GetItem(arrayIdx);
                    Aws::Internal::Endpoint::EndpointAuthScheme currentAuthScheme;
                    for (const auto& mapItemProperty : property.GetAllObjects())
                    {
                        if (mapItemProperty.first == "name") {
                            currentAuthScheme.SetName(CrtToSdkSignerName(mapItemProperty.second.AsString()));
                        } else if (mapItemProperty.first == "signingName") {
                            currentAuthScheme.SetSigningName(mapItemProperty.second.AsString());
                        } else if (mapItemProperty.first == "signingRegion") {
                            currentAuthScheme.SetSigningRegion(mapItemProperty.second.AsString());
                        } else if (mapItemProperty.first == "signingRegionSet") {
                            Aws::Utils::Array<Utils::Json::JsonView> signingRegionArray = mapItemProperty.second.AsArray();
                            if (signingRegionArray.GetLength() != 1) {
                                AWS_LOG_WARN(ENDPOINT_AUTH_SCHEME_TAG,
                                             "Signing region set size is not equal to 1");
                            }
                            if (signingRegionArray.GetLength() > 0) {
                                currentAuthScheme.SetSigningRegionSet(signingRegionArray.GetItem(0).AsString());
                            }
                        } else if (mapItemProperty.first == "disableDoubleEncoding") {
                            currentAuthScheme.SetDisableDoubleEncoding(mapItemProperty.second.AsBool());
                        } else {
                            AWS_LOG_WARN(ENDPOINT_AUTH_SCHEME_TAG, Aws::String("Unknown Endpoint authSchemes attribute property: " + mapItemProperty.first).c_str());
                        }
                    }
                    /* Can't decide if both (i.e. SigV4 and Bearer is present, fail in debug and use first resolved by rules */
                    assert(GetAuthSchemePriority(currentAuthScheme.GetName()) != GetAuthSchemePriority(authScheme.GetName()));
                    if (GetAuthSchemePriority(currentAuthScheme.GetName()) > GetAuthSchemePriority(authScheme.GetName()))
                    {
                        authScheme = std::move(currentAuthScheme);
                    }
                }
            } else if (mapItemAttribute.first == "backend" && mapItemAttribute.second.IsString()) {
                attributes.backend = mapItemAttribute.second.AsString();
            } else if (mapItemAttribute.first == "useS3ExpressSessionAuth" && mapItemAttribute.second.IsBool()) {
                attributes.useS3ExpressAuth = mapItemAttribute.second.AsBool();
            } else {
                AWS_LOG_WARN(ENDPOINT_AUTH_SCHEME_TAG, Aws::String("Unknown Endpoint Attribute: " + mapItemAttribute.first).c_str());
            }
        }
    }
    else
    {
        AWS_LOGSTREAM_ERROR(ENDPOINT_AUTH_SCHEME_TAG, "Json Parse failed with message: " << jsonObject.GetErrorMessage());
    }

    return attributes;
}