// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "azure/storage/common/storage_credential.hpp"

#include <algorithm>

namespace Azure { namespace Storage { namespace _internal {

  ConnectionStringParts ParseConnectionString(const std::string& connectionString)
  {
    std::map<std::string, std::string> connectionStringMap;

    std::string::const_iterator cur = connectionString.begin();

    while (cur != connectionString.end())
    {
      auto key_begin = cur;
      auto key_end = std::find(cur, connectionString.end(), '=');
      std::string key = std::string(key_begin, key_end);
      cur = key_end;
      if (cur != connectionString.end())
      {
        ++cur;
      }

      auto value_begin = cur;
      auto value_end = std::find(cur, connectionString.end(), ';');
      std::string value = std::string(value_begin, value_end);
      cur = value_end;
      if (cur != connectionString.end())
      {
        ++cur;
      }

      if (!key.empty() || !value.empty())
      {
        connectionStringMap[std::move(key)] = std::move(value);
      }
    }

    auto getWithDefault = [](const std::map<std::string, std::string>& m,
                             const std::string& key,
                             const std::string& defaultValue = std::string()) {
      auto ite = m.find(key);
      return ite == m.end() ? defaultValue : ite->second;
    };

    ConnectionStringParts connectionStringParts;

    std::string defaultEndpointsProtocol
        = getWithDefault(connectionStringMap, "DefaultEndpointsProtocol", "https");
    std::string EndpointSuffix
        = getWithDefault(connectionStringMap, "EndpointSuffix", "core.windows.net");
    std::string accountName = getWithDefault(connectionStringMap, "AccountName");
    connectionStringParts.AccountName = accountName;

    std::string endpoint = getWithDefault(connectionStringMap, "BlobEndpoint");
    if (endpoint.empty() && !accountName.empty())
    {
      endpoint = defaultEndpointsProtocol + "://" + accountName + ".blob." + EndpointSuffix;
    }
    connectionStringParts.BlobServiceUrl = Azure::Core::Url(std::move(endpoint));

    endpoint = getWithDefault(connectionStringMap, "DfsEndpoint");
    if (endpoint.empty() && !accountName.empty())
    {
      endpoint = defaultEndpointsProtocol + "://" + accountName + ".dfs." + EndpointSuffix;
    }
    connectionStringParts.DataLakeServiceUrl = Azure::Core::Url(std::move(endpoint));

    endpoint = getWithDefault(connectionStringMap, "FileEndpoint");
    if (endpoint.empty() && !accountName.empty())
    {
      endpoint = defaultEndpointsProtocol + "://" + accountName + ".file." + EndpointSuffix;
    }
    connectionStringParts.FileServiceUrl = Azure::Core::Url(std::move(endpoint));

    endpoint = getWithDefault(connectionStringMap, "QueueEndpoint");
    if (endpoint.empty() && !accountName.empty())
    {
      endpoint = defaultEndpointsProtocol + "://" + accountName + ".queue." + EndpointSuffix;
    }
    connectionStringParts.QueueServiceUrl = Azure::Core::Url(std::move(endpoint));

    std::string accountKey = getWithDefault(connectionStringMap, "AccountKey");
    connectionStringParts.AccountKey = accountKey;
    if (!accountKey.empty())
    {
      if (accountName.empty())
      {
        throw std::runtime_error("Cannot find account name in connection string.");
      }
      connectionStringParts.KeyCredential
          = std::make_shared<StorageSharedKeyCredential>(accountName, accountKey);
    }

    std::string sas = getWithDefault(connectionStringMap, "SharedAccessSignature");
    if (!sas.empty())
    {
      if (sas[0] != '?')
      {
        sas = '?' + sas;
      }
      connectionStringParts.BlobServiceUrl
          = Azure::Core::Url(connectionStringParts.BlobServiceUrl.GetAbsoluteUrl() + sas);
      connectionStringParts.DataLakeServiceUrl
          = Azure::Core::Url(connectionStringParts.DataLakeServiceUrl.GetAbsoluteUrl() + sas);
      connectionStringParts.FileServiceUrl
          = Azure::Core::Url(connectionStringParts.FileServiceUrl.GetAbsoluteUrl() + sas);
      connectionStringParts.QueueServiceUrl
          = Azure::Core::Url(connectionStringParts.QueueServiceUrl.GetAbsoluteUrl() + sas);
    }

    return connectionStringParts;
  }

  std::string GetDefaultScopeForAudience(const std::string& audience)
  {
    if (!audience.empty() && audience.back() == '/')
    {
      return audience + ".default";
    }
    return audience + "/.default";
  }
}}} // namespace Azure::Storage::_internal
