// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#pragma once

#include <azure/core/base64.hpp>
#include <azure/core/cryptography/hash.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace Azure { namespace Storage {

  /**
   * @brief The class for the CRC64 hash function which maps binary data of an arbitrary length to
   * small binary data of a fixed length.
   */
  class Crc64Hash final : public Azure::Core::Cryptography::Hash {
  public:
    /**
     * @brief Concatenates another #Crc64 instance after this instance. This operation has the same
     * effect as if the data in the other instance was append to this instance.
     *
     * @param other Another #Azure::Storage::Crc64Hash instance to be concatenated after this
     * instance.
     */
    void Concatenate(const Crc64Hash& other);

    ~Crc64Hash() override = default;

  private:
    uint64_t m_context = 0ULL;
    uint64_t m_length = 0ULL;

    void OnAppend(const uint8_t* data, size_t length) override;
    std::vector<uint8_t> OnFinal(const uint8_t* data, size_t length) override;
  };

  namespace _internal {
    std::vector<uint8_t> HmacSha256(
        const std::vector<uint8_t>& data,
        const std::vector<uint8_t>& key);
    std::string UrlEncodeQueryParameter(const std::string& value);
    std::string UrlEncodePath(const std::string& value);
  } // namespace _internal
}} // namespace Azure::Storage
