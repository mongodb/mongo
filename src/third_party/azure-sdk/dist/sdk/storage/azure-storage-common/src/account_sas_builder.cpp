// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "azure/storage/common/account_sas_builder.hpp"

#include "azure/storage/common/crypt.hpp"

#include <azure/core/http/http.hpp>

namespace Azure { namespace Storage { namespace Sas {
  namespace {
    constexpr static const char* SasVersion = "2024-11-04";
  }

  void AccountSasBuilder::SetPermissions(AccountSasPermissions permissions)
  {
    Permissions.clear();
    if ((permissions & AccountSasPermissions::Read) == AccountSasPermissions::Read)
    {
      Permissions += "r";
    }
    if ((permissions & AccountSasPermissions::Write) == AccountSasPermissions::Write)
    {
      Permissions += "w";
    }
    if ((permissions & AccountSasPermissions::Delete) == AccountSasPermissions::Delete)
    {
      Permissions += "d";
    }
    if ((permissions & AccountSasPermissions::DeleteVersion)
        == AccountSasPermissions::DeleteVersion)
    {
      Permissions += "x";
    }
    if ((permissions & AccountSasPermissions::PermanentDelete)
        == AccountSasPermissions::PermanentDelete)
    {
      Permissions += "y";
    }
    if ((permissions & AccountSasPermissions::List) == AccountSasPermissions::List)
    {
      Permissions += "l";
    }
    if ((permissions & AccountSasPermissions::Add) == AccountSasPermissions::Add)
    {
      Permissions += "a";
    }
    if ((permissions & AccountSasPermissions::Create) == AccountSasPermissions::Create)
    {
      Permissions += "c";
    }
    if ((permissions & AccountSasPermissions::Update) == AccountSasPermissions::Update)
    {
      Permissions += "u";
    }
    if ((permissions & AccountSasPermissions::Process) == AccountSasPermissions::Process)
    {
      Permissions += "p";
    }
    if ((permissions & AccountSasPermissions::SetImmutabilityPolicy)
        == AccountSasPermissions::SetImmutabilityPolicy)
    {
      Permissions += "i";
    }
    if ((permissions & AccountSasPermissions::Tags) == AccountSasPermissions::Tags)
    {
      Permissions += "t";
    }
    if ((permissions & AccountSasPermissions::Filter) == AccountSasPermissions::Filter)
    {
      Permissions += "f";
    }
  }

  std::string AccountSasBuilder::GenerateSasToken(const StorageSharedKeyCredential& credential)
  {
    std::string protocol = _detail::SasProtocolToString(Protocol);

    std::string services;
    if ((Services & AccountSasServices::Blobs) == AccountSasServices::Blobs)
    {
      services += "b";
    }
    if ((Services & AccountSasServices::Queue) == AccountSasServices::Queue)
    {
      services += "q";
    }
    if ((Services & AccountSasServices::Files) == AccountSasServices::Files)
    {
      services += "f";
    }

    std::string resourceTypes;
    if ((ResourceTypes & AccountSasResource::Service) == AccountSasResource::Service)
    {
      resourceTypes += "s";
    }
    if ((ResourceTypes & AccountSasResource::Container) == AccountSasResource::Container)
    {
      resourceTypes += "c";
    }
    if ((ResourceTypes & AccountSasResource::Object) == AccountSasResource::Object)
    {
      resourceTypes += "o";
    }

    std::string startsOnStr = StartsOn.HasValue()
        ? StartsOn.Value().ToString(
            Azure::DateTime::DateFormat::Rfc3339, Azure::DateTime::TimeFractionFormat::Truncate)
        : "";
    std::string expiresOnStr = ExpiresOn.ToString(
        Azure::DateTime::DateFormat::Rfc3339, Azure::DateTime::TimeFractionFormat::Truncate);

    std::string stringToSign = credential.AccountName + "\n" + Permissions + "\n" + services + "\n"
        + resourceTypes + "\n" + startsOnStr + "\n" + expiresOnStr + "\n"
        + (IPRange.HasValue() ? IPRange.Value() : "") + "\n" + protocol + "\n" + SasVersion + "\n"
        + EncryptionScope + "\n";

    std::string signature = Azure::Core::Convert::Base64Encode(_internal::HmacSha256(
        std::vector<uint8_t>(stringToSign.begin(), stringToSign.end()),
        Azure::Core::Convert::Base64Decode(credential.GetAccountKey())));

    Azure::Core::Url builder;
    builder.AppendQueryParameter("sv", _internal::UrlEncodeQueryParameter(SasVersion));
    builder.AppendQueryParameter("ss", _internal::UrlEncodeQueryParameter(services));
    builder.AppendQueryParameter("srt", _internal::UrlEncodeQueryParameter(resourceTypes));
    builder.AppendQueryParameter("sp", _internal::UrlEncodeQueryParameter(Permissions));
    if (!startsOnStr.empty())
    {
      builder.AppendQueryParameter("st", startsOnStr);
    }
    builder.AppendQueryParameter("se", expiresOnStr);
    if (IPRange.HasValue())
    {
      builder.AppendQueryParameter("sip", _internal::UrlEncodeQueryParameter(IPRange.Value()));
    }
    builder.AppendQueryParameter("spr", _internal::UrlEncodeQueryParameter(protocol));
    builder.AppendQueryParameter("sig", _internal::UrlEncodeQueryParameter(signature));
    if (!EncryptionScope.empty())
    {
      builder.AppendQueryParameter("ses", _internal::UrlEncodeQueryParameter(EncryptionScope));
    }

    return builder.GetAbsoluteUrl();
  }

  std::string AccountSasBuilder::GenerateSasStringToSign(
      const StorageSharedKeyCredential& credential)
  {
    std::string protocol = _detail::SasProtocolToString(Protocol);

    std::string services;
    if ((Services & AccountSasServices::Blobs) == AccountSasServices::Blobs)
    {
      services += "b";
    }
    if ((Services & AccountSasServices::Queue) == AccountSasServices::Queue)
    {
      services += "q";
    }
    if ((Services & AccountSasServices::Files) == AccountSasServices::Files)
    {
      services += "f";
    }

    std::string resourceTypes;
    if ((ResourceTypes & AccountSasResource::Service) == AccountSasResource::Service)
    {
      resourceTypes += "s";
    }
    if ((ResourceTypes & AccountSasResource::Container) == AccountSasResource::Container)
    {
      resourceTypes += "c";
    }
    if ((ResourceTypes & AccountSasResource::Object) == AccountSasResource::Object)
    {
      resourceTypes += "o";
    }

    std::string startsOnStr = StartsOn.HasValue()
        ? StartsOn.Value().ToString(
            Azure::DateTime::DateFormat::Rfc3339, Azure::DateTime::TimeFractionFormat::Truncate)
        : "";
    std::string expiresOnStr = ExpiresOn.ToString(
        Azure::DateTime::DateFormat::Rfc3339, Azure::DateTime::TimeFractionFormat::Truncate);

    return credential.AccountName + "\n" + Permissions + "\n" + services + "\n" + resourceTypes
        + "\n" + startsOnStr + "\n" + expiresOnStr + "\n"
        + (IPRange.HasValue() ? IPRange.Value() : "") + "\n" + protocol + "\n" + SasVersion + "\n"
        + EncryptionScope + "\n";
  }

}}} // namespace Azure::Storage::Sas
