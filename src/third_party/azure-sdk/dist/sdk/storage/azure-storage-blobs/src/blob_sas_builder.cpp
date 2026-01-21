// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "azure/storage/blobs/blob_sas_builder.hpp"

#include <azure/core/http/http.hpp>
#include <azure/storage/common/crypt.hpp>

/* cSpell:ignore rscc, rscd, rsce, rscl, rsct, skoid, sktid */

namespace Azure { namespace Storage { namespace Sas {

  namespace {
    constexpr static const char* SasVersion = Blobs::_detail::ApiVersion;

    std::string BlobSasResourceToString(BlobSasResource resource)
    {
      if (resource == BlobSasResource::BlobContainer)
      {
        return "c";
      }
      else if (resource == BlobSasResource::Blob)
      {
        return "b";
      }
      else if (resource == BlobSasResource::BlobSnapshot)
      {
        return "bs";
      }
      else if (resource == BlobSasResource::BlobVersion)
      {
        return "bv";
      }
      else
      {
        throw std::invalid_argument("Unknown BlobSasResource value.");
      }
    }
  } // namespace

  void BlobSasBuilder::SetPermissions(BlobContainerSasPermissions permissions)
  {
    Permissions.clear();
    // The order matters
    if ((permissions & BlobContainerSasPermissions::Read) == BlobContainerSasPermissions::Read)
    {
      Permissions += "r";
    }
    if ((permissions & BlobContainerSasPermissions::Add) == BlobContainerSasPermissions::Add)
    {
      Permissions += "a";
    }
    if ((permissions & BlobContainerSasPermissions::Create) == BlobContainerSasPermissions::Create)
    {
      Permissions += "c";
    }
    if ((permissions & BlobContainerSasPermissions::Write) == BlobContainerSasPermissions::Write)
    {
      Permissions += "w";
    }
    if ((permissions & BlobContainerSasPermissions::Delete) == BlobContainerSasPermissions::Delete)
    {
      Permissions += "d";
    }
    if ((permissions & BlobContainerSasPermissions::DeleteVersion)
        == BlobContainerSasPermissions::DeleteVersion)
    {
      Permissions += "x";
    }
    if ((permissions & BlobContainerSasPermissions::PermanentDelete)
        == BlobContainerSasPermissions::PermanentDelete)
    {
      Permissions += "y";
    }
    if ((permissions & BlobContainerSasPermissions::List) == BlobContainerSasPermissions::List)
    {
      Permissions += "l";
    }
    if ((permissions & BlobContainerSasPermissions::Tags) == BlobContainerSasPermissions::Tags)
    {
      Permissions += "t";
    }
    if ((permissions & BlobContainerSasPermissions::SetImmutabilityPolicy)
        == BlobContainerSasPermissions::SetImmutabilityPolicy)
    {
      Permissions += "i";
    }
  }

  void BlobSasBuilder::SetPermissions(BlobSasPermissions permissions)
  {
    Permissions.clear();
    // The order matters
    if ((permissions & BlobSasPermissions::Read) == BlobSasPermissions::Read)
    {
      Permissions += "r";
    }
    if ((permissions & BlobSasPermissions::Add) == BlobSasPermissions::Add)
    {
      Permissions += "a";
    }
    if ((permissions & BlobSasPermissions::Create) == BlobSasPermissions::Create)
    {
      Permissions += "c";
    }
    if ((permissions & BlobSasPermissions::Write) == BlobSasPermissions::Write)
    {
      Permissions += "w";
    }
    if ((permissions & BlobSasPermissions::Delete) == BlobSasPermissions::Delete)
    {
      Permissions += "d";
    }
    if ((permissions & BlobSasPermissions::DeleteVersion) == BlobSasPermissions::DeleteVersion)
    {
      Permissions += "x";
    }
    if ((permissions & BlobSasPermissions::PermanentDelete) == BlobSasPermissions::PermanentDelete)
    {
      Permissions += "y";
    }
    if ((permissions & BlobSasPermissions::Tags) == BlobSasPermissions::Tags)
    {
      Permissions += "t";
    }
    if ((permissions & BlobSasPermissions::SetImmutabilityPolicy)
        == BlobSasPermissions::SetImmutabilityPolicy)
    {
      Permissions += "i";
    }
  }

  std::string BlobSasBuilder::GenerateSasToken(const StorageSharedKeyCredential& credential)
  {
    std::string canonicalName = "/blob/" + credential.AccountName + "/" + BlobContainerName;
    if (Resource == BlobSasResource::Blob || Resource == BlobSasResource::BlobSnapshot
        || Resource == BlobSasResource::BlobVersion)
    {
      canonicalName += "/" + BlobName;
    }
    std::string protocol = _detail::SasProtocolToString(Protocol);
    std::string resource = BlobSasResourceToString(Resource);

    std::string snapshotVersion;
    if (Resource == BlobSasResource::BlobSnapshot)
    {
      snapshotVersion = Snapshot;
    }
    else if (Resource == BlobSasResource::BlobVersion)
    {
      snapshotVersion = BlobVersionId;
    }

    std::string startsOnStr = StartsOn.HasValue()
        ? StartsOn.Value().ToString(
            Azure::DateTime::DateFormat::Rfc3339, Azure::DateTime::TimeFractionFormat::Truncate)
        : "";
    std::string expiresOnStr = Identifier.empty()
        ? ExpiresOn.ToString(
            Azure::DateTime::DateFormat::Rfc3339, Azure::DateTime::TimeFractionFormat::Truncate)
        : "";

    std::string stringToSign = Permissions + "\n" + startsOnStr + "\n" + expiresOnStr + "\n"
        + canonicalName + "\n" + Identifier + "\n" + (IPRange.HasValue() ? IPRange.Value() : "")
        + "\n" + protocol + "\n" + SasVersion + "\n" + resource + "\n" + snapshotVersion + "\n"
        + EncryptionScope + "\n" + CacheControl + "\n" + ContentDisposition + "\n" + ContentEncoding
        + "\n" + ContentLanguage + "\n" + ContentType;

    std::string signature = Azure::Core::Convert::Base64Encode(_internal::HmacSha256(
        std::vector<uint8_t>(stringToSign.begin(), stringToSign.end()),
        Azure::Core::Convert::Base64Decode(credential.GetAccountKey())));

    Azure::Core::Url builder;
    builder.AppendQueryParameter("sv", _internal::UrlEncodeQueryParameter(SasVersion));
    builder.AppendQueryParameter("spr", _internal::UrlEncodeQueryParameter(protocol));
    if (!startsOnStr.empty())
    {
      builder.AppendQueryParameter("st", _internal::UrlEncodeQueryParameter(startsOnStr));
    }
    if (!expiresOnStr.empty())
    {
      builder.AppendQueryParameter("se", _internal::UrlEncodeQueryParameter(expiresOnStr));
    }
    if (IPRange.HasValue())
    {
      builder.AppendQueryParameter("sip", _internal::UrlEncodeQueryParameter(IPRange.Value()));
    }
    if (!Identifier.empty())
    {
      builder.AppendQueryParameter("si", _internal::UrlEncodeQueryParameter(Identifier));
    }
    builder.AppendQueryParameter("sr", _internal::UrlEncodeQueryParameter(resource));
    if (!Permissions.empty())
    {
      builder.AppendQueryParameter("sp", _internal::UrlEncodeQueryParameter(Permissions));
    }
    builder.AppendQueryParameter("sig", _internal::UrlEncodeQueryParameter(signature));
    if (!CacheControl.empty())
    {
      builder.AppendQueryParameter("rscc", _internal::UrlEncodeQueryParameter(CacheControl));
    }
    if (!ContentDisposition.empty())
    {
      builder.AppendQueryParameter("rscd", _internal::UrlEncodeQueryParameter(ContentDisposition));
    }
    if (!ContentEncoding.empty())
    {
      builder.AppendQueryParameter("rsce", _internal::UrlEncodeQueryParameter(ContentEncoding));
    }
    if (!ContentLanguage.empty())
    {
      builder.AppendQueryParameter("rscl", _internal::UrlEncodeQueryParameter(ContentLanguage));
    }
    if (!ContentType.empty())
    {
      builder.AppendQueryParameter("rsct", _internal::UrlEncodeQueryParameter(ContentType));
    }
    if (!EncryptionScope.empty())
    {
      builder.AppendQueryParameter("ses", _internal::UrlEncodeQueryParameter(EncryptionScope));
    }

    return builder.GetAbsoluteUrl();
  }

  std::string BlobSasBuilder::GenerateSasToken(
      const Blobs::Models::UserDelegationKey& userDelegationKey,
      const std::string& accountName)
  {
    std::string canonicalName = "/blob/" + accountName + "/" + BlobContainerName;
    if (Resource == BlobSasResource::Blob || Resource == BlobSasResource::BlobSnapshot
        || Resource == BlobSasResource::BlobVersion)
    {
      canonicalName += "/" + BlobName;
    }
    std::string protocol = _detail::SasProtocolToString(Protocol);
    std::string resource = BlobSasResourceToString(Resource);

    std::string snapshotVersion;
    if (Resource == BlobSasResource::BlobSnapshot)
    {
      snapshotVersion = Snapshot;
    }
    else if (Resource == BlobSasResource::BlobVersion)
    {
      snapshotVersion = BlobVersionId;
    }

    std::string startsOnStr = StartsOn.HasValue()
        ? StartsOn.Value().ToString(
            Azure::DateTime::DateFormat::Rfc3339, Azure::DateTime::TimeFractionFormat::Truncate)
        : "";
    std::string expiresOnStr = ExpiresOn.ToString(
        Azure::DateTime::DateFormat::Rfc3339, Azure::DateTime::TimeFractionFormat::Truncate);
    std::string signedStartsOnStr = userDelegationKey.SignedStartsOn.ToString(
        Azure::DateTime::DateFormat::Rfc3339, Azure::DateTime::TimeFractionFormat::Truncate);
    std::string signedExpiresOnStr = userDelegationKey.SignedExpiresOn.ToString(
        Azure::DateTime::DateFormat::Rfc3339, Azure::DateTime::TimeFractionFormat::Truncate);

    std::string stringToSign = Permissions + "\n" + startsOnStr + "\n" + expiresOnStr + "\n"
        + canonicalName + "\n" + userDelegationKey.SignedObjectId + "\n"
        + userDelegationKey.SignedTenantId + "\n" + signedStartsOnStr + "\n" + signedExpiresOnStr
        + "\n" + userDelegationKey.SignedService + "\n" + userDelegationKey.SignedVersion
        + "\n\n\n\n" + (IPRange.HasValue() ? IPRange.Value() : "") + "\n" + protocol + "\n"
        + SasVersion + "\n" + resource + "\n" + snapshotVersion + "\n" + EncryptionScope + "\n"
        + CacheControl + "\n" + ContentDisposition + "\n" + ContentEncoding + "\n" + ContentLanguage
        + "\n" + ContentType;

    std::string signature = Azure::Core::Convert::Base64Encode(_internal::HmacSha256(
        std::vector<uint8_t>(stringToSign.begin(), stringToSign.end()),
        Azure::Core::Convert::Base64Decode(userDelegationKey.Value)));

    Azure::Core::Url builder;
    builder.AppendQueryParameter("sv", _internal::UrlEncodeQueryParameter(SasVersion));
    builder.AppendQueryParameter("sr", _internal::UrlEncodeQueryParameter(resource));
    if (!startsOnStr.empty())
    {
      builder.AppendQueryParameter("st", _internal::UrlEncodeQueryParameter(startsOnStr));
    }
    builder.AppendQueryParameter("se", _internal::UrlEncodeQueryParameter(expiresOnStr));
    builder.AppendQueryParameter("sp", _internal::UrlEncodeQueryParameter(Permissions));
    if (IPRange.HasValue())
    {
      builder.AppendQueryParameter("sip", _internal::UrlEncodeQueryParameter(IPRange.Value()));
    }
    builder.AppendQueryParameter("spr", _internal::UrlEncodeQueryParameter(protocol));
    builder.AppendQueryParameter(
        "skoid", _internal::UrlEncodeQueryParameter(userDelegationKey.SignedObjectId));
    builder.AppendQueryParameter(
        "sktid", _internal::UrlEncodeQueryParameter(userDelegationKey.SignedTenantId));
    builder.AppendQueryParameter("skt", _internal::UrlEncodeQueryParameter(signedStartsOnStr));
    builder.AppendQueryParameter("ske", _internal::UrlEncodeQueryParameter(signedExpiresOnStr));
    builder.AppendQueryParameter(
        "sks", _internal::UrlEncodeQueryParameter(userDelegationKey.SignedService));
    builder.AppendQueryParameter(
        "skv", _internal::UrlEncodeQueryParameter(userDelegationKey.SignedVersion));
    if (!CacheControl.empty())
    {
      builder.AppendQueryParameter("rscc", _internal::UrlEncodeQueryParameter(CacheControl));
    }
    if (!ContentDisposition.empty())
    {
      builder.AppendQueryParameter("rscd", _internal::UrlEncodeQueryParameter(ContentDisposition));
    }
    if (!ContentEncoding.empty())
    {
      builder.AppendQueryParameter("rsce", _internal::UrlEncodeQueryParameter(ContentEncoding));
    }
    if (!ContentLanguage.empty())
    {
      builder.AppendQueryParameter("rscl", _internal::UrlEncodeQueryParameter(ContentLanguage));
    }
    if (!ContentType.empty())
    {
      builder.AppendQueryParameter("rsct", _internal::UrlEncodeQueryParameter(ContentType));
    }
    if (!EncryptionScope.empty())
    {
      builder.AppendQueryParameter("ses", _internal::UrlEncodeQueryParameter(EncryptionScope));
    }
    builder.AppendQueryParameter("sig", _internal::UrlEncodeQueryParameter(signature));

    return builder.GetAbsoluteUrl();
  }

  std::string BlobSasBuilder::GenerateSasStringToSign(const StorageSharedKeyCredential& credential)
  {
    std::string canonicalName = "/blob/" + credential.AccountName + "/" + BlobContainerName;
    if (Resource == BlobSasResource::Blob || Resource == BlobSasResource::BlobSnapshot
        || Resource == BlobSasResource::BlobVersion)
    {
      canonicalName += "/" + BlobName;
    }
    std::string protocol = _detail::SasProtocolToString(Protocol);
    std::string resource = BlobSasResourceToString(Resource);

    std::string snapshotVersion;
    if (Resource == BlobSasResource::BlobSnapshot)
    {
      snapshotVersion = Snapshot;
    }
    else if (Resource == BlobSasResource::BlobVersion)
    {
      snapshotVersion = BlobVersionId;
    }

    std::string startsOnStr = StartsOn.HasValue()
        ? StartsOn.Value().ToString(
            Azure::DateTime::DateFormat::Rfc3339, Azure::DateTime::TimeFractionFormat::Truncate)
        : "";
    std::string expiresOnStr = Identifier.empty()
        ? ExpiresOn.ToString(
            Azure::DateTime::DateFormat::Rfc3339, Azure::DateTime::TimeFractionFormat::Truncate)
        : "";

    return Permissions + "\n" + startsOnStr + "\n" + expiresOnStr + "\n" + canonicalName + "\n"
        + Identifier + "\n" + (IPRange.HasValue() ? IPRange.Value() : "") + "\n" + protocol + "\n"
        + SasVersion + "\n" + resource + "\n" + snapshotVersion + "\n" + EncryptionScope + "\n"
        + CacheControl + "\n" + ContentDisposition + "\n" + ContentEncoding + "\n" + ContentLanguage
        + "\n" + ContentType;
  }

  std::string BlobSasBuilder::GenerateSasStringToSign(
      const Blobs::Models::UserDelegationKey& userDelegationKey,
      const std::string& accountName)
  {
    std::string canonicalName = "/blob/" + accountName + "/" + BlobContainerName;
    if (Resource == BlobSasResource::Blob || Resource == BlobSasResource::BlobSnapshot
        || Resource == BlobSasResource::BlobVersion)
    {
      canonicalName += "/" + BlobName;
    }
    std::string protocol = _detail::SasProtocolToString(Protocol);
    std::string resource = BlobSasResourceToString(Resource);

    std::string snapshotVersion;
    if (Resource == BlobSasResource::BlobSnapshot)
    {
      snapshotVersion = Snapshot;
    }
    else if (Resource == BlobSasResource::BlobVersion)
    {
      snapshotVersion = BlobVersionId;
    }

    std::string startsOnStr = StartsOn.HasValue()
        ? StartsOn.Value().ToString(
            Azure::DateTime::DateFormat::Rfc3339, Azure::DateTime::TimeFractionFormat::Truncate)
        : "";
    std::string expiresOnStr = ExpiresOn.ToString(
        Azure::DateTime::DateFormat::Rfc3339, Azure::DateTime::TimeFractionFormat::Truncate);
    std::string signedStartsOnStr = userDelegationKey.SignedStartsOn.ToString(
        Azure::DateTime::DateFormat::Rfc3339, Azure::DateTime::TimeFractionFormat::Truncate);
    std::string signedExpiresOnStr = userDelegationKey.SignedExpiresOn.ToString(
        Azure::DateTime::DateFormat::Rfc3339, Azure::DateTime::TimeFractionFormat::Truncate);

    return Permissions + "\n" + startsOnStr + "\n" + expiresOnStr + "\n" + canonicalName + "\n"
        + userDelegationKey.SignedObjectId + "\n" + userDelegationKey.SignedTenantId + "\n"
        + signedStartsOnStr + "\n" + signedExpiresOnStr + "\n" + userDelegationKey.SignedService
        + "\n" + userDelegationKey.SignedVersion + "\n\n\n\n"
        + (IPRange.HasValue() ? IPRange.Value() : "") + "\n" + protocol + "\n" + SasVersion + "\n"
        + resource + "\n" + snapshotVersion + "\n" + EncryptionScope + "\n" + CacheControl + "\n"
        + ContentDisposition + "\n" + ContentEncoding + "\n" + ContentLanguage + "\n" + ContentType;
  }

}}} // namespace Azure::Storage::Sas
