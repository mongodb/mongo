// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

/**
 * @file
 * @brief Utility functions to help compute the hash value for the input binary data, using
 * algorithms such as MD5.
 */

#pragma once

#include "azure/core/azure_assert.hpp"

#include <memory>
#include <stdexcept>
#include <stdint.h>
#include <string>
#include <vector>

namespace Azure { namespace Core { namespace Cryptography {

  /**
   * @brief Represents the base class for hash algorithms which map binary data of an arbitrary
   * length to small binary data of a fixed length.
   */
  class Hash {
  private:
    /**
     * @brief Used to append partial binary input data to compute the hash in a streaming fashion.
     * @remark Once all the data has been added, call #Azure::Core::Cryptography::Hash::Final() to
     * get the computed hash value.
     * @param data The pointer to the current block of binary data that is used for hash
     * calculation.
     * @param length The size of the data provided.
     */
    virtual void OnAppend(const uint8_t* data, size_t length) = 0;

    /**
     * @brief Computes the hash value of the specified binary input data, including any previously
     * appended.
     * @param data The pointer to binary data to compute the hash value for.
     * @param length The size of the data provided.
     * @return The computed hash value corresponding to the input provided including any previously
     * appended.
     */
    virtual std::vector<uint8_t> OnFinal(const uint8_t* data, size_t length) = 0;

  protected:
    /**
     * @brief Constructs a default instance of `%Hash`.
     *
     */
    Hash() = default;

  public:
    /**
     * @brief Used to append partial binary input data to compute the hash in a streaming fashion.
     * @remark Once all the data has been added, call #Azure::Core::Cryptography::Hash::Final() to
     * get the computed hash value.
     * @remark Do not call this function after a call to #Azure::Core::Cryptography::Hash::Final().
     * @param data The pointer to the current block of binary data that is used for hash
     * calculation.
     * @param length The size of the data provided.
     */
    void Append(const uint8_t* data, size_t length)
    {
      AZURE_ASSERT(data || length == 0);
      AZURE_ASSERT_MSG(!m_isDone, "Cannot call Append after calling Final().");
      OnAppend(data, length);
    }

    /**
     * @brief Computes the hash value of the specified binary input data, including any previously
     * appended.
     * @remark Do not call this function multiple times.
     * @param data The pointer to the last block of binary data to compute the hash value for.
     * @param length The size of the data provided.
     * @return The computed hash value corresponding to the input provided, including any previously
     * appended.
     */
    std::vector<uint8_t> Final(const uint8_t* data, size_t length)
    {
      AZURE_ASSERT(data || length == 0);
      AZURE_ASSERT_MSG(!m_isDone, "Cannot call Final() multiple times.");
      m_isDone = true;
      return OnFinal(data, length);
    }

    /**
     * @brief Computes the hash value of all the binary input data appended to the instance so far.
     * @remark Use #Azure::Core::Cryptography::Hash::Append() to add more partial data before
     * calling this function.
     * @remark Do not call this function multiple times.
     * @return The computed hash value corresponding to the input provided.
     */
    std::vector<uint8_t> Final() { return Final(nullptr, 0); }

    /**
     * @brief Destructs `%Hash`.
     *
     */
    virtual ~Hash() = default;

  private:
    bool m_isDone = false;

    /**
     * @brief `%Hash` does not allow copy construction.
     *
     */
    Hash(Hash const&) = delete;

    /**
     * @brief `%Hash` does not allow assignment.
     *
     */
    void operator=(Hash const&) = delete;
  };

  /**
   * @brief Represents the class for the MD5 hash function which maps binary data of an arbitrary
   * length to small binary data of a fixed length.
   *
   * @warning MD5 is a deprecated hashing algorithm and SHOULD NOT be used,
   * unless it is used to implement a specific protocol (See RFC 6151 for more information
   * about the weaknesses of the MD5 hash function). Client implementers should strongly prefer the
   * SHA256, SHA384, and SHA512 hash functions.
   */
  class Md5Hash final : public Hash {

  public:
    /**
     * @brief Construct a default instance of #Azure::Core::Cryptography::Md5Hash.
     *
     */
    Md5Hash();

    /**
     * @brief Destructs `%Md5Hash`.
     *
     */
    ~Md5Hash() override;

  private:
    std::unique_ptr<Hash> m_implementation;

    /**
     * @brief Computes the hash value of the specified binary input data, including any previously
     * appended.
     * @param data The pointer to binary data to compute the hash value for.
     * @param length The size of the data provided.
     * @return The computed MD5 hash value corresponding to the input provided including any
     * previously appended.
     */
    std::vector<uint8_t> OnFinal(const uint8_t* data, size_t length) override;

    /**
     * @brief Used to append partial binary input data to compute the MD5 hash in a streaming
     * fashion.
     * @remark Once all the data has been added, call #Azure::Core::Cryptography::Hash::Final() to
     * get the computed hash value.
     * @param data The pointer to the current block of binary data that is used for hash
     * calculation.
     * @param length The size of the data provided.
     */
    void OnAppend(const uint8_t* data, size_t length) override;
  };

}}} // namespace Azure::Core::Cryptography
