// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

/**
 * @brief Compute the hash value for the input binary data, using
 * SHA256, SHA384 and SHA512.
 *
 */

#pragma once

#include <azure/core/cryptography/hash.hpp>

#include <memory>
#include <string>
#include <vector>

namespace Azure { namespace Core { namespace Cryptography { namespace _internal {

  /**
   * @brief Defines #Sha1Hash.
   *
   * @warning SHA1 is a deprecated hashing algorithm and SHOULD NOT be used,
   * unless it is used to implement a specific protocol (for instance, RFC 6455 and
   * RFC 7517 both require the use of SHA1 hashes). SHA256, SHA384, and SHA512 are all preferred to
   * SHA1.
   *
   */
  class Sha1Hash final : public Azure::Core::Cryptography::Hash {
  public:
    /**
     * @brief Construct a default instance of #Sha1Hash.
     *
     */
    Sha1Hash();

    /**
     * @brief Cleanup any state when destroying the instance of #Sha1Hash.
     *
     */
    ~Sha1Hash() {}

  private:
    /**
     * @brief Underlying implementation based on the OS.
     *
     */
    std::unique_ptr<Azure::Core::Cryptography::Hash> m_portableImplementation;

    /**
     * @brief Computes the hash value of the specified binary input data, including any previously
     * appended.
     * @param data The pointer to binary data to compute the hash value for.
     * @param length The size of the data provided.
     * @return The computed SHA1 hash value corresponding to the input provided including any
     * previously appended.
     */
    std::vector<uint8_t> OnFinal(const uint8_t* data, size_t length) override
    {
      return m_portableImplementation->Final(data, length);
    }

    /**
     * @brief Used to append partial binary input data to compute the SHA1 hash in a streaming
     * fashion.
     * @remark Once all the data has been added, call #Final() to get the computed hash value.
     * @param data The pointer to the current block of binary data that is used for hash
     * calculation.
     * @param length The size of the data provided.
     */
    void OnAppend(const uint8_t* data, size_t length) override
    {
      return m_portableImplementation->Append(data, length);
    }
  };
  /**
   * @brief Defines #Sha256Hash.
   *
   */
  class Sha256Hash final : public Azure::Core::Cryptography::Hash {
  public:
    /**
     * @brief Construct a default instance of #Sha256Hash.
     *
     */
    Sha256Hash();

    /**
     * @brief Cleanup any state when destroying the instance of #Sha256Hash.
     *
     */
    ~Sha256Hash() {}

  private:
    /**
     * @brief Underline implementation based on the OS.
     *
     */
    std::unique_ptr<Azure::Core::Cryptography::Hash> m_portableImplementation;

    /**
     * @brief Computes the hash value of the specified binary input data, including any previously
     * appended.
     * @param data The pointer to binary data to compute the hash value for.
     * @param length The size of the data provided.
     * @return The computed SHA256 hash value corresponding to the input provided including any
     * previously appended.
     */
    std::vector<uint8_t> OnFinal(const uint8_t* data, size_t length) override
    {
      return m_portableImplementation->Final(data, length);
    }

    /**
     * @brief Used to append partial binary input data to compute the SHA256 hash in a streaming
     * fashion.
     * @remark Once all the data has been added, call #Final() to get the computed hash value.
     * @param data The pointer to the current block of binary data that is used for hash
     * calculation.
     * @param length The size of the data provided.
     */
    void OnAppend(const uint8_t* data, size_t length) override
    {
      return m_portableImplementation->Append(data, length);
    }
  };

  /**
   * @brief Defines #Sha384Hash.
   *
   */
  class Sha384Hash final : public Azure::Core::Cryptography::Hash {
  public:
    /**
     * @brief Construct a default instance of #Sha384Hash.
     *
     */
    Sha384Hash();

    /**
     * @brief Cleanup any state when destroying the instance of #Sha384Hash.
     *
     */
    ~Sha384Hash() {}

  private:
    /**
     * @brief Underline implementation based on the OS.
     *
     */
    std::unique_ptr<Azure::Core::Cryptography::Hash> m_portableImplementation;

    /**
     * @brief Computes the hash value of the specified binary input data, including any previously
     * appended.
     * @param data The pointer to binary data to compute the hash value for.
     * @param length The size of the data provided.
     * @return The computed SHA384 hash value corresponding to the input provided including any
     * previously appended.
     */
    std::vector<uint8_t> OnFinal(const uint8_t* data, size_t length) override
    {
      return m_portableImplementation->Final(data, length);
    }

    /**
     * @brief Used to append partial binary input data to compute the SHA384 hash in a streaming
     * fashion.
     * @remark Once all the data has been added, call #Final() to get the computed hash value.
     * @param data The pointer to the current block of binary data that is used for hash
     * calculation.
     * @param length The size of the data provided.
     */
    void OnAppend(const uint8_t* data, size_t length) override
    {
      return m_portableImplementation->Append(data, length);
    }
  };

  /**
   * @brief Defines #Sha512Hash.
   *
   */
  class Sha512Hash final : public Azure::Core::Cryptography::Hash {
  public:
    /**
     * @brief Construct a default instance of #Sha512Hash.
     *
     */
    Sha512Hash();

    /**
     * @brief Cleanup any state when destroying the instance of #Sha512Hash.
     *
     */
    ~Sha512Hash() {}

  private:
    /**
     * @brief Underline implementation based on the OS.
     *
     */
    std::unique_ptr<Azure::Core::Cryptography::Hash> m_portableImplementation;

    /**
     * @brief Computes the hash value of the specified binary input data, including any previously
     * appended.
     * @param data The pointer to binary data to compute the hash value for.
     * @param length The size of the data provided.
     * @return The computed SHA512 hash value corresponding to the input provided including any
     * previously appended.
     */
    std::vector<uint8_t> OnFinal(const uint8_t* data, size_t length) override
    {
      return m_portableImplementation->Final(data, length);
    }

    /**
     * @brief Used to append partial binary input data to compute the SHA512 hash in a streaming
     * fashion.
     * @remark Once all the data has been added, call #Final() to get the computed hash value.
     * @param data The pointer to the current block of binary data that is used for hash
     * calculation.
     * @param length The size of the data provided.
     */
    void OnAppend(const uint8_t* data, size_t length) override
    {
      return m_portableImplementation->Append(data, length);
    }
  };

}}}} // namespace Azure::Core::Cryptography::_internal
