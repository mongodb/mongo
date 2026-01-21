// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#pragma once

#include <azure/core/http/http.hpp>

#include <memory>
#include <mutex>
#include <string>

namespace Azure { namespace Storage {

  namespace Sas {
    struct AccountSasBuilder;
    struct BlobSasBuilder;
    struct ShareSasBuilder;
    struct DataLakeSasBuilder;
    struct QueueSasBuilder;
  } // namespace Sas

  namespace _internal {
    class SharedKeyPolicy;
  }

  /**
   * @brief A StorageSharedKeyCredential is a credential backed by a storage account's name and
   * one of its access keys.
   */
  class StorageSharedKeyCredential final {
  public:
    /**
     * @brief Initializes a new instance of the StorageSharedKeyCredential.
     *
     * @param accountName Name of the storage account.
     * @param accountKey Access key of the storage
     * account.
     */
    explicit StorageSharedKeyCredential(std::string accountName, std::string accountKey)
        : AccountName(std::move(accountName)), m_accountKey(std::move(accountKey))
    {
    }

    /**
     * @brief Update the storage account's access key. This intended to be used when you've
     * regenerated your storage account's access keys and want to update long lived clients.
     *
     * @param accountKey A storage account access key.
     */
    void Update(std::string accountKey)
    {
      std::lock_guard<std::mutex> guard(m_mutex);
      m_accountKey = std::move(accountKey);
    }

    /**
     * @brief Gets the name of the Storage Account.
     */
    const std::string AccountName;

  private:
    friend class _internal::SharedKeyPolicy;
    friend struct Sas::BlobSasBuilder;
    friend struct Sas::ShareSasBuilder;
    friend struct Sas::DataLakeSasBuilder;
    friend struct Sas::QueueSasBuilder;
    friend struct Sas::AccountSasBuilder;
    std::string GetAccountKey() const
    {
      std::lock_guard<std::mutex> guard(m_mutex);
      return m_accountKey;
    }

    mutable std::mutex m_mutex;
    std::string m_accountKey;
  };

  namespace _internal {

    struct ConnectionStringParts
    {
      std::string AccountName;
      std::string AccountKey;
      Azure::Core::Url BlobServiceUrl;
      Azure::Core::Url FileServiceUrl;
      Azure::Core::Url QueueServiceUrl;
      Azure::Core::Url DataLakeServiceUrl;
      std::shared_ptr<StorageSharedKeyCredential> KeyCredential;
    };

    ConnectionStringParts ParseConnectionString(const std::string& connectionString);

    std::string GetDefaultScopeForAudience(const std::string& audience);

  } // namespace _internal

}} // namespace Azure::Storage
