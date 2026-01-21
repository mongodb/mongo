// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "azure/storage/common/crypt.hpp"

#include <azure/core/platform.hpp>

#if defined(AZ_PLATFORM_WINDOWS)
#if !defined(NOMINMAX)
#define NOMINMAX
#endif
// Windows needs to go before bcrypt
#include <windows.h>

#include <bcrypt.h>
#elif defined(AZ_PLATFORM_POSIX)
#include <openssl/bio.h>
#include <openssl/buffer.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>
#endif

#include "azure/storage/common/storage_common.hpp"

#include <azure/core/http/http.hpp>

#include <algorithm>
#include <stdexcept>
#include <vector>

namespace Azure { namespace Storage {

  namespace _internal {
    static const char* Subdelimiters = "!$&'()*+,;=";

    std::string UrlEncodeQueryParameter(const std::string& value)
    {
      const static std::string DoNotEncodeCharacters = []() {
        // Core::Url::Encode won't encode unreserved characters.
        std::string doNotEncodeCharacters = Subdelimiters;
        doNotEncodeCharacters += "/:@?";
        doNotEncodeCharacters.erase(
            std::remove_if(
                doNotEncodeCharacters.begin(),
                doNotEncodeCharacters.end(),
                [](char x) {
                  // we also encode + and &
                  // Surprisingly, '=' also needs to be encoded because Azure Storage server side is
                  // so strict. We are applying this function to query key and value respectively,
                  // so this won't affect that = used to separate key and query.
                  return x == '+' || x == '=' || x == '&';
                }),
            doNotEncodeCharacters.end());
        return doNotEncodeCharacters;
      }();
      return Core::Url::Encode(value, DoNotEncodeCharacters);
    }

    std::string UrlEncodePath(const std::string& value)
    {
      const static std::string DoNotEncodeCharacters = []() {
        // Core::Url::Encode won't encode unreserved characters.
        std::string doNotEncodeCharacters = Subdelimiters;
        doNotEncodeCharacters += "/:@";
        doNotEncodeCharacters.erase(
            std::remove_if(
                doNotEncodeCharacters.begin(),
                doNotEncodeCharacters.end(),
                [](char x) {
                  // we also encode +
                  return x == '+';
                }),
            doNotEncodeCharacters.end());
        return doNotEncodeCharacters;
      }();
      return Core::Url::Encode(value, DoNotEncodeCharacters);
    }
  } // namespace _internal

#if defined(AZ_PLATFORM_WINDOWS)

  namespace _internal {

    enum class AlgorithmType
    {
      HmacSha256,
    };

    struct AlgorithmProviderInstance final
    {
      BCRYPT_ALG_HANDLE Handle;
      size_t ContextSize;
      size_t HashLength;

      AlgorithmProviderInstance(AlgorithmType type)
      {
        const wchar_t* algorithmId = nullptr;
        if (type == AlgorithmType::HmacSha256)
        {
          algorithmId = BCRYPT_SHA256_ALGORITHM;
        }
        else
        {
          throw std::runtime_error("Unknown algorithm type.");
        }

        unsigned long algorithmFlags = 0;
        if (type == AlgorithmType::HmacSha256)
        {
          algorithmFlags = BCRYPT_ALG_HANDLE_HMAC_FLAG;
        }
        NTSTATUS status
            = BCryptOpenAlgorithmProvider(&Handle, algorithmId, nullptr, algorithmFlags);
        if (!BCRYPT_SUCCESS(status))
        {
          throw std::runtime_error("BCryptOpenAlgorithmProvider failed.");
        }
        DWORD objectLength = 0;
        DWORD dataLength = 0;
        status = BCryptGetProperty(
            Handle,
            BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PBYTE>(&objectLength),
            sizeof(objectLength),
            &dataLength,
            0);
        if (!BCRYPT_SUCCESS(status))
        {
          throw std::runtime_error("BCryptGetProperty failed.");
        }
        ContextSize = objectLength;
        DWORD hashLength = 0;
        status = BCryptGetProperty(
            Handle,
            BCRYPT_HASH_LENGTH,
            reinterpret_cast<PBYTE>(&hashLength),
            sizeof(hashLength),
            &dataLength,
            0);
        if (!BCRYPT_SUCCESS(status))
        {
          throw std::runtime_error("BCryptGetProperty failed.");
        }
        HashLength = hashLength;
      }

      ~AlgorithmProviderInstance() { BCryptCloseAlgorithmProvider(Handle, 0); }
    };

    std::vector<uint8_t> HmacSha256(
        const std::vector<uint8_t>& data,
        const std::vector<uint8_t>& key)
    {
      AZURE_ASSERT_MSG(data.size() <= (std::numeric_limits<ULONG>::max)(), "Data size is too big.");

      static AlgorithmProviderInstance AlgorithmProvider(AlgorithmType::HmacSha256);

      std::string context;
      context.resize(AlgorithmProvider.ContextSize);

      BCRYPT_HASH_HANDLE hashHandle;
      NTSTATUS status = BCryptCreateHash(
          AlgorithmProvider.Handle,
          &hashHandle,
          reinterpret_cast<PUCHAR>(&context[0]),
          static_cast<ULONG>(context.size()),
          reinterpret_cast<PUCHAR>(const_cast<uint8_t*>(&key[0])),
          static_cast<ULONG>(key.size()),
          0);
      if (!BCRYPT_SUCCESS(status))
      {
        throw std::runtime_error("BCryptCreateHash failed.");
      }

      status = BCryptHashData(
          hashHandle,
          reinterpret_cast<PBYTE>(const_cast<uint8_t*>(data.data())),
          static_cast<ULONG>(data.size()),
          0);
      if (!BCRYPT_SUCCESS(status))
      {
        throw std::runtime_error("BCryptHashData failed.");
      }

      std::vector<uint8_t> hash;
      hash.resize(AlgorithmProvider.HashLength);
      status = BCryptFinishHash(
          hashHandle, reinterpret_cast<PUCHAR>(&hash[0]), static_cast<ULONG>(hash.size()), 0);
      if (!BCRYPT_SUCCESS(status))
      {
        throw std::runtime_error("BCryptFinishHash failed.");
      }

      BCryptDestroyHash(hashHandle);

      return hash;
    }
  } // namespace _internal

#elif defined(AZ_PLATFORM_POSIX)

  namespace _internal {

    std::vector<uint8_t> HmacSha256(
        const std::vector<uint8_t>& data,
        const std::vector<uint8_t>& key)
    {
      uint8_t hash[EVP_MAX_MD_SIZE];
      unsigned int hashLength = 0;
      HMAC(
          EVP_sha256(),
          key.data(),
          static_cast<int>(key.size()),
          reinterpret_cast<const unsigned char*>(data.data()),
          data.size(),
          reinterpret_cast<unsigned char*>(&hash[0]),
          &hashLength);

      return std::vector<uint8_t>(std::begin(hash), std::begin(hash) + hashLength);
    }

  } // namespace _internal

#endif

  static constexpr uint64_t Crc64Poly = 0x9A6C9329AC4BC9B5ULL;
  static constexpr uint64_t Crc64MU1[] = {
      0x0000000000000000ULL, 0x7f6ef0c830358979ULL, 0xfedde190606b12f2ULL, 0x81b31158505e9b8bULL,
      0xc962e5739841b68fULL, 0xb60c15bba8743ff6ULL, 0x37bf04e3f82aa47dULL, 0x48d1f42bc81f2d04ULL,
      0xa61cecb46814fe75ULL, 0xd9721c7c5821770cULL, 0x58c10d24087fec87ULL, 0x27affdec384a65feULL,
      0x6f7e09c7f05548faULL, 0x1010f90fc060c183ULL, 0x91a3e857903e5a08ULL, 0xeecd189fa00bd371ULL,
      0x78e0ff3b88be6f81ULL, 0x078e0ff3b88be6f8ULL, 0x863d1eabe8d57d73ULL, 0xf953ee63d8e0f40aULL,
      0xb1821a4810ffd90eULL, 0xceecea8020ca5077ULL, 0x4f5ffbd87094cbfcULL, 0x30310b1040a14285ULL,
      0xdefc138fe0aa91f4ULL, 0xa192e347d09f188dULL, 0x2021f21f80c18306ULL, 0x5f4f02d7b0f40a7fULL,
      0x179ef6fc78eb277bULL, 0x68f0063448deae02ULL, 0xe943176c18803589ULL, 0x962de7a428b5bcf0ULL,
      0xf1c1fe77117cdf02ULL, 0x8eaf0ebf2149567bULL, 0x0f1c1fe77117cdf0ULL, 0x7072ef2f41224489ULL,
      0x38a31b04893d698dULL, 0x47cdebccb908e0f4ULL, 0xc67efa94e9567b7fULL, 0xb9100a5cd963f206ULL,
      0x57dd12c379682177ULL, 0x28b3e20b495da80eULL, 0xa900f35319033385ULL, 0xd66e039b2936bafcULL,
      0x9ebff7b0e12997f8ULL, 0xe1d10778d11c1e81ULL, 0x606216208142850aULL, 0x1f0ce6e8b1770c73ULL,
      0x8921014c99c2b083ULL, 0xf64ff184a9f739faULL, 0x77fce0dcf9a9a271ULL, 0x08921014c99c2b08ULL,
      0x4043e43f0183060cULL, 0x3f2d14f731b68f75ULL, 0xbe9e05af61e814feULL, 0xc1f0f56751dd9d87ULL,
      0x2f3dedf8f1d64ef6ULL, 0x50531d30c1e3c78fULL, 0xd1e00c6891bd5c04ULL, 0xae8efca0a188d57dULL,
      0xe65f088b6997f879ULL, 0x9931f84359a27100ULL, 0x1882e91b09fcea8bULL, 0x67ec19d339c963f2ULL,
      0xd75adabd7a6e2d6fULL, 0xa8342a754a5ba416ULL, 0x29873b2d1a053f9dULL, 0x56e9cbe52a30b6e4ULL,
      0x1e383fcee22f9be0ULL, 0x6156cf06d21a1299ULL, 0xe0e5de5e82448912ULL, 0x9f8b2e96b271006bULL,
      0x71463609127ad31aULL, 0x0e28c6c1224f5a63ULL, 0x8f9bd7997211c1e8ULL, 0xf0f5275142244891ULL,
      0xb824d37a8a3b6595ULL, 0xc74a23b2ba0eececULL, 0x46f932eaea507767ULL, 0x3997c222da65fe1eULL,
      0xafba2586f2d042eeULL, 0xd0d4d54ec2e5cb97ULL, 0x5167c41692bb501cULL, 0x2e0934dea28ed965ULL,
      0x66d8c0f56a91f461ULL, 0x19b6303d5aa47d18ULL, 0x980521650afae693ULL, 0xe76bd1ad3acf6feaULL,
      0x09a6c9329ac4bc9bULL, 0x76c839faaaf135e2ULL, 0xf77b28a2faafae69ULL, 0x8815d86aca9a2710ULL,
      0xc0c42c4102850a14ULL, 0xbfaadc8932b0836dULL, 0x3e19cdd162ee18e6ULL, 0x41773d1952db919fULL,
      0x269b24ca6b12f26dULL, 0x59f5d4025b277b14ULL, 0xd846c55a0b79e09fULL, 0xa72835923b4c69e6ULL,
      0xeff9c1b9f35344e2ULL, 0x90973171c366cd9bULL, 0x1124202993385610ULL, 0x6e4ad0e1a30ddf69ULL,
      0x8087c87e03060c18ULL, 0xffe938b633338561ULL, 0x7e5a29ee636d1eeaULL, 0x0134d92653589793ULL,
      0x49e52d0d9b47ba97ULL, 0x368bddc5ab7233eeULL, 0xb738cc9dfb2ca865ULL, 0xc8563c55cb19211cULL,
      0x5e7bdbf1e3ac9decULL, 0x21152b39d3991495ULL, 0xa0a63a6183c78f1eULL, 0xdfc8caa9b3f20667ULL,
      0x97193e827bed2b63ULL, 0xe877ce4a4bd8a21aULL, 0x69c4df121b863991ULL, 0x16aa2fda2bb3b0e8ULL,
      0xf86737458bb86399ULL, 0x8709c78dbb8deae0ULL, 0x06bad6d5ebd3716bULL, 0x79d4261ddbe6f812ULL,
      0x3105d23613f9d516ULL, 0x4e6b22fe23cc5c6fULL, 0xcfd833a67392c7e4ULL, 0xb0b6c36e43a74e9dULL,
      0x9a6c9329ac4bc9b5ULL, 0xe50263e19c7e40ccULL, 0x64b172b9cc20db47ULL, 0x1bdf8271fc15523eULL,
      0x530e765a340a7f3aULL, 0x2c608692043ff643ULL, 0xadd397ca54616dc8ULL, 0xd2bd67026454e4b1ULL,
      0x3c707f9dc45f37c0ULL, 0x431e8f55f46abeb9ULL, 0xc2ad9e0da4342532ULL, 0xbdc36ec59401ac4bULL,
      0xf5129aee5c1e814fULL, 0x8a7c6a266c2b0836ULL, 0x0bcf7b7e3c7593bdULL, 0x74a18bb60c401ac4ULL,
      0xe28c6c1224f5a634ULL, 0x9de29cda14c02f4dULL, 0x1c518d82449eb4c6ULL, 0x633f7d4a74ab3dbfULL,
      0x2bee8961bcb410bbULL, 0x548079a98c8199c2ULL, 0xd53368f1dcdf0249ULL, 0xaa5d9839ecea8b30ULL,
      0x449080a64ce15841ULL, 0x3bfe706e7cd4d138ULL, 0xba4d61362c8a4ab3ULL, 0xc52391fe1cbfc3caULL,
      0x8df265d5d4a0eeceULL, 0xf29c951de49567b7ULL, 0x732f8445b4cbfc3cULL, 0x0c41748d84fe7545ULL,
      0x6bad6d5ebd3716b7ULL, 0x14c39d968d029fceULL, 0x95708ccedd5c0445ULL, 0xea1e7c06ed698d3cULL,
      0xa2cf882d2576a038ULL, 0xdda178e515432941ULL, 0x5c1269bd451db2caULL, 0x237c997575283bb3ULL,
      0xcdb181ead523e8c2ULL, 0xb2df7122e51661bbULL, 0x336c607ab548fa30ULL, 0x4c0290b2857d7349ULL,
      0x04d364994d625e4dULL, 0x7bbd94517d57d734ULL, 0xfa0e85092d094cbfULL, 0x856075c11d3cc5c6ULL,
      0x134d926535897936ULL, 0x6c2362ad05bcf04fULL, 0xed9073f555e26bc4ULL, 0x92fe833d65d7e2bdULL,
      0xda2f7716adc8cfb9ULL, 0xa54187de9dfd46c0ULL, 0x24f29686cda3dd4bULL, 0x5b9c664efd965432ULL,
      0xb5517ed15d9d8743ULL, 0xca3f8e196da80e3aULL, 0x4b8c9f413df695b1ULL, 0x34e26f890dc31cc8ULL,
      0x7c339ba2c5dc31ccULL, 0x035d6b6af5e9b8b5ULL, 0x82ee7a32a5b7233eULL, 0xfd808afa9582aa47ULL,
      0x4d364994d625e4daULL, 0x3258b95ce6106da3ULL, 0xb3eba804b64ef628ULL, 0xcc8558cc867b7f51ULL,
      0x8454ace74e645255ULL, 0xfb3a5c2f7e51db2cULL, 0x7a894d772e0f40a7ULL, 0x05e7bdbf1e3ac9deULL,
      0xeb2aa520be311aafULL, 0x944455e88e0493d6ULL, 0x15f744b0de5a085dULL, 0x6a99b478ee6f8124ULL,
      0x224840532670ac20ULL, 0x5d26b09b16452559ULL, 0xdc95a1c3461bbed2ULL, 0xa3fb510b762e37abULL,
      0x35d6b6af5e9b8b5bULL, 0x4ab846676eae0222ULL, 0xcb0b573f3ef099a9ULL, 0xb465a7f70ec510d0ULL,
      0xfcb453dcc6da3dd4ULL, 0x83daa314f6efb4adULL, 0x0269b24ca6b12f26ULL, 0x7d0742849684a65fULL,
      0x93ca5a1b368f752eULL, 0xeca4aad306bafc57ULL, 0x6d17bb8b56e467dcULL, 0x12794b4366d1eea5ULL,
      0x5aa8bf68aecec3a1ULL, 0x25c64fa09efb4ad8ULL, 0xa4755ef8cea5d153ULL, 0xdb1bae30fe90582aULL,
      0xbcf7b7e3c7593bd8ULL, 0xc399472bf76cb2a1ULL, 0x422a5673a732292aULL, 0x3d44a6bb9707a053ULL,
      0x759552905f188d57ULL, 0x0afba2586f2d042eULL, 0x8b48b3003f739fa5ULL, 0xf42643c80f4616dcULL,
      0x1aeb5b57af4dc5adULL, 0x6585ab9f9f784cd4ULL, 0xe436bac7cf26d75fULL, 0x9b584a0fff135e26ULL,
      0xd389be24370c7322ULL, 0xace74eec0739fa5bULL, 0x2d545fb4576761d0ULL, 0x523aaf7c6752e8a9ULL,
      0xc41748d84fe75459ULL, 0xbb79b8107fd2dd20ULL, 0x3acaa9482f8c46abULL, 0x45a459801fb9cfd2ULL,
      0x0d75adabd7a6e2d6ULL, 0x721b5d63e7936bafULL, 0xf3a84c3bb7cdf024ULL, 0x8cc6bcf387f8795dULL,
      0x620ba46c27f3aa2cULL, 0x1d6554a417c62355ULL, 0x9cd645fc4798b8deULL, 0xe3b8b53477ad31a7ULL,
      0xab69411fbfb21ca3ULL, 0xd407b1d78f8795daULL, 0x55b4a08fdfd90e51ULL, 0x2ada5047efec8728ULL,
  };

  static constexpr uint64_t Crc64MU32[] = {
      0x0000000000000000ULL, 0xb8c533c1177eb231ULL, 0x455341d1766af709ULL, 0xfd96721061144538ULL,
      0x8aa683a2ecd5ee12ULL, 0x3263b063fbab5c23ULL, 0xcff5c2739abf191bULL, 0x7730f1b28dc1ab2aULL,
      0x21942116813c4f4fULL, 0x995112d79642fd7eULL, 0x64c760c7f756b846ULL, 0xdc025306e0280a77ULL,
      0xab32a2b46de9a15dULL, 0x13f791757a97136cULL, 0xee61e3651b835654ULL, 0x56a4d0a40cfde465ULL,
      0x4328422d02789e9eULL, 0xfbed71ec15062cafULL, 0x067b03fc74126997ULL, 0xbebe303d636cdba6ULL,
      0xc98ec18feead708cULL, 0x714bf24ef9d3c2bdULL, 0x8cdd805e98c78785ULL, 0x3418b39f8fb935b4ULL,
      0x62bc633b8344d1d1ULL, 0xda7950fa943a63e0ULL, 0x27ef22eaf52e26d8ULL, 0x9f2a112be25094e9ULL,
      0xe81ae0996f913fc3ULL, 0x50dfd35878ef8df2ULL, 0xad49a14819fbc8caULL, 0x158c92890e857afbULL,
      0x8650845a04f13d3cULL, 0x3e95b79b138f8f0dULL, 0xc303c58b729bca35ULL, 0x7bc6f64a65e57804ULL,
      0x0cf607f8e824d32eULL, 0xb4333439ff5a611fULL, 0x49a546299e4e2427ULL, 0xf16075e889309616ULL,
      0xa7c4a54c85cd7273ULL, 0x1f01968d92b3c042ULL, 0xe297e49df3a7857aULL, 0x5a52d75ce4d9374bULL,
      0x2d6226ee69189c61ULL, 0x95a7152f7e662e50ULL, 0x6831673f1f726b68ULL, 0xd0f454fe080cd959ULL,
      0xc578c6770689a3a2ULL, 0x7dbdf5b611f71193ULL, 0x802b87a670e354abULL, 0x38eeb467679de69aULL,
      0x4fde45d5ea5c4db0ULL, 0xf71b7614fd22ff81ULL, 0x0a8d04049c36bab9ULL, 0xb24837c58b480888ULL,
      0xe4ece76187b5ecedULL, 0x5c29d4a090cb5edcULL, 0xa1bfa6b0f1df1be4ULL, 0x197a9571e6a1a9d5ULL,
      0x6e4a64c36b6002ffULL, 0xd68f57027c1eb0ceULL, 0x2b1925121d0af5f6ULL, 0x93dc16d30a7447c7ULL,
      0x38782ee75175e913ULL, 0x80bd1d26460b5b22ULL, 0x7d2b6f36271f1e1aULL, 0xc5ee5cf73061ac2bULL,
      0xb2dead45bda00701ULL, 0x0a1b9e84aadeb530ULL, 0xf78dec94cbcaf008ULL, 0x4f48df55dcb44239ULL,
      0x19ec0ff1d049a65cULL, 0xa1293c30c737146dULL, 0x5cbf4e20a6235155ULL, 0xe47a7de1b15de364ULL,
      0x934a8c533c9c484eULL, 0x2b8fbf922be2fa7fULL, 0xd619cd824af6bf47ULL, 0x6edcfe435d880d76ULL,
      0x7b506cca530d778dULL, 0xc3955f0b4473c5bcULL, 0x3e032d1b25678084ULL, 0x86c61eda321932b5ULL,
      0xf1f6ef68bfd8999fULL, 0x4933dca9a8a62baeULL, 0xb4a5aeb9c9b26e96ULL, 0x0c609d78deccdca7ULL,
      0x5ac44ddcd23138c2ULL, 0xe2017e1dc54f8af3ULL, 0x1f970c0da45bcfcbULL, 0xa7523fccb3257dfaULL,
      0xd062ce7e3ee4d6d0ULL, 0x68a7fdbf299a64e1ULL, 0x95318faf488e21d9ULL, 0x2df4bc6e5ff093e8ULL,
      0xbe28aabd5584d42fULL, 0x06ed997c42fa661eULL, 0xfb7beb6c23ee2326ULL, 0x43bed8ad34909117ULL,
      0x348e291fb9513a3dULL, 0x8c4b1adeae2f880cULL, 0x71dd68cecf3bcd34ULL, 0xc9185b0fd8457f05ULL,
      0x9fbc8babd4b89b60ULL, 0x2779b86ac3c62951ULL, 0xdaefca7aa2d26c69ULL, 0x622af9bbb5acde58ULL,
      0x151a0809386d7572ULL, 0xaddf3bc82f13c743ULL, 0x504949d84e07827bULL, 0xe88c7a195979304aULL,
      0xfd00e89057fc4ab1ULL, 0x45c5db514082f880ULL, 0xb853a9412196bdb8ULL, 0x00969a8036e80f89ULL,
      0x77a66b32bb29a4a3ULL, 0xcf6358f3ac571692ULL, 0x32f52ae3cd4353aaULL, 0x8a301922da3de19bULL,
      0xdc94c986d6c005feULL, 0x6451fa47c1beb7cfULL, 0x99c78857a0aaf2f7ULL, 0x2102bb96b7d440c6ULL,
      0x56324a243a15ebecULL, 0xeef779e52d6b59ddULL, 0x13610bf54c7f1ce5ULL, 0xaba438345b01aed4ULL,
      0x70f05dcea2ebd226ULL, 0xc8356e0fb5956017ULL, 0x35a31c1fd481252fULL, 0x8d662fdec3ff971eULL,
      0xfa56de6c4e3e3c34ULL, 0x4293edad59408e05ULL, 0xbf059fbd3854cb3dULL, 0x07c0ac7c2f2a790cULL,
      0x51647cd823d79d69ULL, 0xe9a14f1934a92f58ULL, 0x14373d0955bd6a60ULL, 0xacf20ec842c3d851ULL,
      0xdbc2ff7acf02737bULL, 0x6307ccbbd87cc14aULL, 0x9e91beabb9688472ULL, 0x26548d6aae163643ULL,
      0x33d81fe3a0934cb8ULL, 0x8b1d2c22b7edfe89ULL, 0x768b5e32d6f9bbb1ULL, 0xce4e6df3c1870980ULL,
      0xb97e9c414c46a2aaULL, 0x01bbaf805b38109bULL, 0xfc2ddd903a2c55a3ULL, 0x44e8ee512d52e792ULL,
      0x124c3ef521af03f7ULL, 0xaa890d3436d1b1c6ULL, 0x571f7f2457c5f4feULL, 0xefda4ce540bb46cfULL,
      0x98eabd57cd7aede5ULL, 0x202f8e96da045fd4ULL, 0xddb9fc86bb101aecULL, 0x657ccf47ac6ea8ddULL,
      0xf6a0d994a61aef1aULL, 0x4e65ea55b1645d2bULL, 0xb3f39845d0701813ULL, 0x0b36ab84c70eaa22ULL,
      0x7c065a364acf0108ULL, 0xc4c369f75db1b339ULL, 0x39551be73ca5f601ULL, 0x819028262bdb4430ULL,
      0xd734f8822726a055ULL, 0x6ff1cb4330581264ULL, 0x9267b953514c575cULL, 0x2aa28a924632e56dULL,
      0x5d927b20cbf34e47ULL, 0xe55748e1dc8dfc76ULL, 0x18c13af1bd99b94eULL, 0xa0040930aae70b7fULL,
      0xb5889bb9a4627184ULL, 0x0d4da878b31cc3b5ULL, 0xf0dbda68d208868dULL, 0x481ee9a9c57634bcULL,
      0x3f2e181b48b79f96ULL, 0x87eb2bda5fc92da7ULL, 0x7a7d59ca3edd689fULL, 0xc2b86a0b29a3daaeULL,
      0x941cbaaf255e3ecbULL, 0x2cd9896e32208cfaULL, 0xd14ffb7e5334c9c2ULL, 0x698ac8bf444a7bf3ULL,
      0x1eba390dc98bd0d9ULL, 0xa67f0accdef562e8ULL, 0x5be978dcbfe127d0ULL, 0xe32c4b1da89f95e1ULL,
      0x48887329f39e3b35ULL, 0xf04d40e8e4e08904ULL, 0x0ddb32f885f4cc3cULL, 0xb51e0139928a7e0dULL,
      0xc22ef08b1f4bd527ULL, 0x7aebc34a08356716ULL, 0x877db15a6921222eULL, 0x3fb8829b7e5f901fULL,
      0x691c523f72a2747aULL, 0xd1d961fe65dcc64bULL, 0x2c4f13ee04c88373ULL, 0x948a202f13b63142ULL,
      0xe3bad19d9e779a68ULL, 0x5b7fe25c89092859ULL, 0xa6e9904ce81d6d61ULL, 0x1e2ca38dff63df50ULL,
      0x0ba03104f1e6a5abULL, 0xb36502c5e698179aULL, 0x4ef370d5878c52a2ULL, 0xf636431490f2e093ULL,
      0x8106b2a61d334bb9ULL, 0x39c381670a4df988ULL, 0xc455f3776b59bcb0ULL, 0x7c90c0b67c270e81ULL,
      0x2a34101270daeae4ULL, 0x92f123d367a458d5ULL, 0x6f6751c306b01dedULL, 0xd7a2620211ceafdcULL,
      0xa09293b09c0f04f6ULL, 0x1857a0718b71b6c7ULL, 0xe5c1d261ea65f3ffULL, 0x5d04e1a0fd1b41ceULL,
      0xced8f773f76f0609ULL, 0x761dc4b2e011b438ULL, 0x8b8bb6a28105f100ULL, 0x334e8563967b4331ULL,
      0x447e74d11bbae81bULL, 0xfcbb47100cc45a2aULL, 0x012d35006dd01f12ULL, 0xb9e806c17aaead23ULL,
      0xef4cd66576534946ULL, 0x5789e5a4612dfb77ULL, 0xaa1f97b40039be4fULL, 0x12daa47517470c7eULL,
      0x65ea55c79a86a754ULL, 0xdd2f66068df81565ULL, 0x20b91416ecec505dULL, 0x987c27d7fb92e26cULL,
      0x8df0b55ef5179897ULL, 0x3535869fe2692aa6ULL, 0xc8a3f48f837d6f9eULL, 0x7066c74e9403ddafULL,
      0x075636fc19c27685ULL, 0xbf93053d0ebcc4b4ULL, 0x4205772d6fa8818cULL, 0xfac044ec78d633bdULL,
      0xac649448742bd7d8ULL, 0x14a1a789635565e9ULL, 0xe937d599024120d1ULL, 0x51f2e658153f92e0ULL,
      0x26c217ea98fe39caULL, 0x9e07242b8f808bfbULL, 0x6391563bee94cec3ULL, 0xdb5465faf9ea7cf2ULL,

      0x0000000000000000ULL, 0xf6f734b768e04748ULL, 0xd9374f3d89571dfbULL, 0x2fc07b8ae1b75ab3ULL,
      0x86b7b8284a39a89dULL, 0x70408c9f22d9efd5ULL, 0x5f80f715c36eb566ULL, 0xa977c3a2ab8ef22eULL,
      0x39b65603cce4c251ULL, 0xcf4162b4a4048519ULL, 0xe081193e45b3dfaaULL, 0x16762d892d5398e2ULL,
      0xbf01ee2b86dd6accULL, 0x49f6da9cee3d2d84ULL, 0x6636a1160f8a7737ULL, 0x90c195a1676a307fULL,
      0x736cac0799c984a2ULL, 0x859b98b0f129c3eaULL, 0xaa5be33a109e9959ULL, 0x5cacd78d787ede11ULL,
      0xf5db142fd3f02c3fULL, 0x032c2098bb106b77ULL, 0x2cec5b125aa731c4ULL, 0xda1b6fa53247768cULL,
      0x4adafa04552d46f3ULL, 0xbc2dceb33dcd01bbULL, 0x93edb539dc7a5b08ULL, 0x651a818eb49a1c40ULL,
      0xcc6d422c1f14ee6eULL, 0x3a9a769b77f4a926ULL, 0x155a0d119643f395ULL, 0xe3ad39a6fea3b4ddULL,
      0xe6d9580f33930944ULL, 0x102e6cb85b734e0cULL, 0x3fee1732bac414bfULL, 0xc9192385d22453f7ULL,
      0x606ee02779aaa1d9ULL, 0x9699d490114ae691ULL, 0xb959af1af0fdbc22ULL, 0x4fae9bad981dfb6aULL,
      0xdf6f0e0cff77cb15ULL, 0x29983abb97978c5dULL, 0x065841317620d6eeULL, 0xf0af75861ec091a6ULL,
      0x59d8b624b54e6388ULL, 0xaf2f8293ddae24c0ULL, 0x80eff9193c197e73ULL, 0x7618cdae54f9393bULL,
      0x95b5f408aa5a8de6ULL, 0x6342c0bfc2bacaaeULL, 0x4c82bb35230d901dULL, 0xba758f824bedd755ULL,
      0x13024c20e063257bULL, 0xe5f5789788836233ULL, 0xca35031d69343880ULL, 0x3cc237aa01d47fc8ULL,
      0xac03a20b66be4fb7ULL, 0x5af496bc0e5e08ffULL, 0x7534ed36efe9524cULL, 0x83c3d98187091504ULL,
      0x2ab41a232c87e72aULL, 0xdc432e944467a062ULL, 0xf383551ea5d0fad1ULL, 0x057461a9cd30bd99ULL,
      0xf96b964d3fb181e3ULL, 0x0f9ca2fa5751c6abULL, 0x205cd970b6e69c18ULL, 0xd6abedc7de06db50ULL,
      0x7fdc2e657588297eULL, 0x892b1ad21d686e36ULL, 0xa6eb6158fcdf3485ULL, 0x501c55ef943f73cdULL,
      0xc0ddc04ef35543b2ULL, 0x362af4f99bb504faULL, 0x19ea8f737a025e49ULL, 0xef1dbbc412e21901ULL,
      0x466a7866b96ceb2fULL, 0xb09d4cd1d18cac67ULL, 0x9f5d375b303bf6d4ULL, 0x69aa03ec58dbb19cULL,
      0x8a073a4aa6780541ULL, 0x7cf00efdce984209ULL, 0x533075772f2f18baULL, 0xa5c741c047cf5ff2ULL,
      0x0cb08262ec41addcULL, 0xfa47b6d584a1ea94ULL, 0xd587cd5f6516b027ULL, 0x2370f9e80df6f76fULL,
      0xb3b16c496a9cc710ULL, 0x454658fe027c8058ULL, 0x6a862374e3cbdaebULL, 0x9c7117c38b2b9da3ULL,
      0x3506d46120a56f8dULL, 0xc3f1e0d6484528c5ULL, 0xec319b5ca9f27276ULL, 0x1ac6afebc112353eULL,
      0x1fb2ce420c2288a7ULL, 0xe945faf564c2cfefULL, 0xc685817f8575955cULL, 0x3072b5c8ed95d214ULL,
      0x9905766a461b203aULL, 0x6ff242dd2efb6772ULL, 0x40323957cf4c3dc1ULL, 0xb6c50de0a7ac7a89ULL,
      0x26049841c0c64af6ULL, 0xd0f3acf6a8260dbeULL, 0xff33d77c4991570dULL, 0x09c4e3cb21711045ULL,
      0xa0b320698affe26bULL, 0x564414dee21fa523ULL, 0x79846f5403a8ff90ULL, 0x8f735be36b48b8d8ULL,
      0x6cde624595eb0c05ULL, 0x9a2956f2fd0b4b4dULL, 0xb5e92d781cbc11feULL, 0x431e19cf745c56b6ULL,
      0xea69da6ddfd2a498ULL, 0x1c9eeedab732e3d0ULL, 0x335e95505685b963ULL, 0xc5a9a1e73e65fe2bULL,
      0x55683446590fce54ULL, 0xa39f00f131ef891cULL, 0x8c5f7b7bd058d3afULL, 0x7aa84fccb8b894e7ULL,
      0xd3df8c6e133666c9ULL, 0x2528b8d97bd62181ULL, 0x0ae8c3539a617b32ULL, 0xfc1ff7e4f2813c7aULL,
      0xc60e0ac927f490adULL, 0x30f93e7e4f14d7e5ULL, 0x1f3945f4aea38d56ULL, 0xe9ce7143c643ca1eULL,
      0x40b9b2e16dcd3830ULL, 0xb64e8656052d7f78ULL, 0x998efddce49a25cbULL, 0x6f79c96b8c7a6283ULL,
      0xffb85ccaeb1052fcULL, 0x094f687d83f015b4ULL, 0x268f13f762474f07ULL, 0xd07827400aa7084fULL,
      0x790fe4e2a129fa61ULL, 0x8ff8d055c9c9bd29ULL, 0xa038abdf287ee79aULL, 0x56cf9f68409ea0d2ULL,
      0xb562a6cebe3d140fULL, 0x43959279d6dd5347ULL, 0x6c55e9f3376a09f4ULL, 0x9aa2dd445f8a4ebcULL,
      0x33d51ee6f404bc92ULL, 0xc5222a519ce4fbdaULL, 0xeae251db7d53a169ULL, 0x1c15656c15b3e621ULL,
      0x8cd4f0cd72d9d65eULL, 0x7a23c47a1a399116ULL, 0x55e3bff0fb8ecba5ULL, 0xa3148b47936e8cedULL,
      0x0a6348e538e07ec3ULL, 0xfc947c525000398bULL, 0xd35407d8b1b76338ULL, 0x25a3336fd9572470ULL,
      0x20d752c6146799e9ULL, 0xd62066717c87dea1ULL, 0xf9e01dfb9d308412ULL, 0x0f17294cf5d0c35aULL,
      0xa660eaee5e5e3174ULL, 0x5097de5936be763cULL, 0x7f57a5d3d7092c8fULL, 0x89a09164bfe96bc7ULL,
      0x196104c5d8835bb8ULL, 0xef963072b0631cf0ULL, 0xc0564bf851d44643ULL, 0x36a17f4f3934010bULL,
      0x9fd6bced92baf325ULL, 0x6921885afa5ab46dULL, 0x46e1f3d01bedeedeULL, 0xb016c767730da996ULL,
      0x53bbfec18dae1d4bULL, 0xa54cca76e54e5a03ULL, 0x8a8cb1fc04f900b0ULL, 0x7c7b854b6c1947f8ULL,
      0xd50c46e9c797b5d6ULL, 0x23fb725eaf77f29eULL, 0x0c3b09d44ec0a82dULL, 0xfacc3d632620ef65ULL,
      0x6a0da8c2414adf1aULL, 0x9cfa9c7529aa9852ULL, 0xb33ae7ffc81dc2e1ULL, 0x45cdd348a0fd85a9ULL,
      0xecba10ea0b737787ULL, 0x1a4d245d639330cfULL, 0x358d5fd782246a7cULL, 0xc37a6b60eac42d34ULL,
      0x3f659c841845114eULL, 0xc992a83370a55606ULL, 0xe652d3b991120cb5ULL, 0x10a5e70ef9f24bfdULL,
      0xb9d224ac527cb9d3ULL, 0x4f25101b3a9cfe9bULL, 0x60e56b91db2ba428ULL, 0x96125f26b3cbe360ULL,
      0x06d3ca87d4a1d31fULL, 0xf024fe30bc419457ULL, 0xdfe485ba5df6cee4ULL, 0x2913b10d351689acULL,
      0x806472af9e987b82ULL, 0x76934618f6783ccaULL, 0x59533d9217cf6679ULL, 0xafa409257f2f2131ULL,
      0x4c093083818c95ecULL, 0xbafe0434e96cd2a4ULL, 0x953e7fbe08db8817ULL, 0x63c94b09603bcf5fULL,
      0xcabe88abcbb53d71ULL, 0x3c49bc1ca3557a39ULL, 0x1389c79642e2208aULL, 0xe57ef3212a0267c2ULL,
      0x75bf66804d6857bdULL, 0x83485237258810f5ULL, 0xac8829bdc43f4a46ULL, 0x5a7f1d0aacdf0d0eULL,
      0xf308dea80751ff20ULL, 0x05ffea1f6fb1b868ULL, 0x2a3f91958e06e2dbULL, 0xdcc8a522e6e6a593ULL,
      0xd9bcc48b2bd6180aULL, 0x2f4bf03c43365f42ULL, 0x008b8bb6a28105f1ULL, 0xf67cbf01ca6142b9ULL,
      0x5f0b7ca361efb097ULL, 0xa9fc4814090ff7dfULL, 0x863c339ee8b8ad6cULL, 0x70cb07298058ea24ULL,
      0xe00a9288e732da5bULL, 0x16fda63f8fd29d13ULL, 0x393dddb56e65c7a0ULL, 0xcfcae902068580e8ULL,
      0x66bd2aa0ad0b72c6ULL, 0x904a1e17c5eb358eULL, 0xbf8a659d245c6f3dULL, 0x497d512a4cbc2875ULL,
      0xaad0688cb21f9ca8ULL, 0x5c275c3bdaffdbe0ULL, 0x73e727b13b488153ULL, 0x8510130653a8c61bULL,
      0x2c67d0a4f8263435ULL, 0xda90e41390c6737dULL, 0xf5509f99717129ceULL, 0x03a7ab2e19916e86ULL,
      0x93663e8f7efb5ef9ULL, 0x65910a38161b19b1ULL, 0x4a5171b2f7ac4302ULL, 0xbca645059f4c044aULL,
      0x15d186a734c2f664ULL, 0xe326b2105c22b12cULL, 0xcce6c99abd95eb9fULL, 0x3a11fd2dd575acd7ULL,

      0x0000000000000000ULL, 0x71b0c13da512335dULL, 0xe361827b4a2466baULL, 0x92d14346ef3655e7ULL,
      0xf21a22a5ccdf5e1fULL, 0x83aae39869cd6d42ULL, 0x117ba0de86fb38a5ULL, 0x60cb61e323e90bf8ULL,
      0xd0ed6318c1292f55ULL, 0xa15da225643b1c08ULL, 0x338ce1638b0d49efULL, 0x423c205e2e1f7ab2ULL,
      0x22f741bd0df6714aULL, 0x53478080a8e44217ULL, 0xc196c3c647d217f0ULL, 0xb02602fbe2c024adULL,
      0x9503e062dac5cdc1ULL, 0xe4b3215f7fd7fe9cULL, 0x7662621990e1ab7bULL, 0x07d2a32435f39826ULL,
      0x6719c2c7161a93deULL, 0x16a903fab308a083ULL, 0x847840bc5c3ef564ULL, 0xf5c88181f92cc639ULL,
      0x45ee837a1bece294ULL, 0x345e4247befed1c9ULL, 0xa68f010151c8842eULL, 0xd73fc03cf4dab773ULL,
      0xb7f4a1dfd733bc8bULL, 0xc64460e272218fd6ULL, 0x549523a49d17da31ULL, 0x2525e2993805e96cULL,
      0x1edee696ed1c08e9ULL, 0x6f6e27ab480e3bb4ULL, 0xfdbf64eda7386e53ULL, 0x8c0fa5d0022a5d0eULL,
      0xecc4c43321c356f6ULL, 0x9d74050e84d165abULL, 0x0fa546486be7304cULL, 0x7e158775cef50311ULL,
      0xce33858e2c3527bcULL, 0xbf8344b3892714e1ULL, 0x2d5207f566114106ULL, 0x5ce2c6c8c303725bULL,
      0x3c29a72be0ea79a3ULL, 0x4d99661645f84afeULL, 0xdf482550aace1f19ULL, 0xaef8e46d0fdc2c44ULL,
      0x8bdd06f437d9c528ULL, 0xfa6dc7c992cbf675ULL, 0x68bc848f7dfda392ULL, 0x190c45b2d8ef90cfULL,
      0x79c72451fb069b37ULL, 0x0877e56c5e14a86aULL, 0x9aa6a62ab122fd8dULL, 0xeb1667171430ced0ULL,
      0x5b3065ecf6f0ea7dULL, 0x2a80a4d153e2d920ULL, 0xb851e797bcd48cc7ULL, 0xc9e126aa19c6bf9aULL,
      0xa92a47493a2fb462ULL, 0xd89a86749f3d873fULL, 0x4a4bc532700bd2d8ULL, 0x3bfb040fd519e185ULL,
      0x3dbdcd2dda3811d2ULL, 0x4c0d0c107f2a228fULL, 0xdedc4f56901c7768ULL, 0xaf6c8e6b350e4435ULL,
      0xcfa7ef8816e74fcdULL, 0xbe172eb5b3f57c90ULL, 0x2cc66df35cc32977ULL, 0x5d76accef9d11a2aULL,
      0xed50ae351b113e87ULL, 0x9ce06f08be030ddaULL, 0x0e312c4e5135583dULL, 0x7f81ed73f4276b60ULL,
      0x1f4a8c90d7ce6098ULL, 0x6efa4dad72dc53c5ULL, 0xfc2b0eeb9dea0622ULL, 0x8d9bcfd638f8357fULL,
      0xa8be2d4f00fddc13ULL, 0xd90eec72a5efef4eULL, 0x4bdfaf344ad9baa9ULL, 0x3a6f6e09efcb89f4ULL,
      0x5aa40feacc22820cULL, 0x2b14ced76930b151ULL, 0xb9c58d918606e4b6ULL, 0xc8754cac2314d7ebULL,
      0x78534e57c1d4f346ULL, 0x09e38f6a64c6c01bULL, 0x9b32cc2c8bf095fcULL, 0xea820d112ee2a6a1ULL,
      0x8a496cf20d0bad59ULL, 0xfbf9adcfa8199e04ULL, 0x6928ee89472fcbe3ULL, 0x18982fb4e23df8beULL,
      0x23632bbb3724193bULL, 0x52d3ea8692362a66ULL, 0xc002a9c07d007f81ULL, 0xb1b268fdd8124cdcULL,
      0xd179091efbfb4724ULL, 0xa0c9c8235ee97479ULL, 0x32188b65b1df219eULL, 0x43a84a5814cd12c3ULL,
      0xf38e48a3f60d366eULL, 0x823e899e531f0533ULL, 0x10efcad8bc2950d4ULL, 0x615f0be5193b6389ULL,
      0x01946a063ad26871ULL, 0x7024ab3b9fc05b2cULL, 0xe2f5e87d70f60ecbULL, 0x93452940d5e43d96ULL,
      0xb660cbd9ede1d4faULL, 0xc7d00ae448f3e7a7ULL, 0x550149a2a7c5b240ULL, 0x24b1889f02d7811dULL,
      0x447ae97c213e8ae5ULL, 0x35ca2841842cb9b8ULL, 0xa71b6b076b1aec5fULL, 0xd6abaa3ace08df02ULL,
      0x668da8c12cc8fbafULL, 0x173d69fc89dac8f2ULL, 0x85ec2aba66ec9d15ULL, 0xf45ceb87c3feae48ULL,
      0x94978a64e017a5b0ULL, 0xe5274b59450596edULL, 0x77f6081faa33c30aULL, 0x0646c9220f21f057ULL,
      0x7b7b9a5bb47023a4ULL, 0x0acb5b66116210f9ULL, 0x981a1820fe54451eULL, 0xe9aad91d5b467643ULL,
      0x8961b8fe78af7dbbULL, 0xf8d179c3ddbd4ee6ULL, 0x6a003a85328b1b01ULL, 0x1bb0fbb89799285cULL,
      0xab96f94375590cf1ULL, 0xda26387ed04b3facULL, 0x48f77b383f7d6a4bULL, 0x3947ba059a6f5916ULL,
      0x598cdbe6b98652eeULL, 0x283c1adb1c9461b3ULL, 0xbaed599df3a23454ULL, 0xcb5d98a056b00709ULL,
      0xee787a396eb5ee65ULL, 0x9fc8bb04cba7dd38ULL, 0x0d19f842249188dfULL, 0x7ca9397f8183bb82ULL,
      0x1c62589ca26ab07aULL, 0x6dd299a107788327ULL, 0xff03dae7e84ed6c0ULL, 0x8eb31bda4d5ce59dULL,
      0x3e951921af9cc130ULL, 0x4f25d81c0a8ef26dULL, 0xddf49b5ae5b8a78aULL, 0xac445a6740aa94d7ULL,
      0xcc8f3b8463439f2fULL, 0xbd3ffab9c651ac72ULL, 0x2feeb9ff2967f995ULL, 0x5e5e78c28c75cac8ULL,
      0x65a57ccd596c2b4dULL, 0x1415bdf0fc7e1810ULL, 0x86c4feb613484df7ULL, 0xf7743f8bb65a7eaaULL,
      0x97bf5e6895b37552ULL, 0xe60f9f5530a1460fULL, 0x74dedc13df9713e8ULL, 0x056e1d2e7a8520b5ULL,
      0xb5481fd598450418ULL, 0xc4f8dee83d573745ULL, 0x56299daed26162a2ULL, 0x27995c93777351ffULL,
      0x47523d70549a5a07ULL, 0x36e2fc4df188695aULL, 0xa433bf0b1ebe3cbdULL, 0xd5837e36bbac0fe0ULL,
      0xf0a69caf83a9e68cULL, 0x81165d9226bbd5d1ULL, 0x13c71ed4c98d8036ULL, 0x6277dfe96c9fb36bULL,
      0x02bcbe0a4f76b893ULL, 0x730c7f37ea648bceULL, 0xe1dd3c710552de29ULL, 0x906dfd4ca040ed74ULL,
      0x204bffb74280c9d9ULL, 0x51fb3e8ae792fa84ULL, 0xc32a7dcc08a4af63ULL, 0xb29abcf1adb69c3eULL,
      0xd251dd128e5f97c6ULL, 0xa3e11c2f2b4da49bULL, 0x31305f69c47bf17cULL, 0x40809e546169c221ULL,
      0x46c657766e483276ULL, 0x3776964bcb5a012bULL, 0xa5a7d50d246c54ccULL, 0xd4171430817e6791ULL,
      0xb4dc75d3a2976c69ULL, 0xc56cb4ee07855f34ULL, 0x57bdf7a8e8b30ad3ULL, 0x260d36954da1398eULL,
      0x962b346eaf611d23ULL, 0xe79bf5530a732e7eULL, 0x754ab615e5457b99ULL, 0x04fa7728405748c4ULL,
      0x643116cb63be433cULL, 0x1581d7f6c6ac7061ULL, 0x875094b0299a2586ULL, 0xf6e0558d8c8816dbULL,
      0xd3c5b714b48dffb7ULL, 0xa2757629119fcceaULL, 0x30a4356ffea9990dULL, 0x4114f4525bbbaa50ULL,
      0x21df95b17852a1a8ULL, 0x506f548cdd4092f5ULL, 0xc2be17ca3276c712ULL, 0xb30ed6f79764f44fULL,
      0x0328d40c75a4d0e2ULL, 0x72981531d0b6e3bfULL, 0xe04956773f80b658ULL, 0x91f9974a9a928505ULL,
      0xf132f6a9b97b8efdULL, 0x808237941c69bda0ULL, 0x125374d2f35fe847ULL, 0x63e3b5ef564ddb1aULL,
      0x5818b1e083543a9fULL, 0x29a870dd264609c2ULL, 0xbb79339bc9705c25ULL, 0xcac9f2a66c626f78ULL,
      0xaa0293454f8b6480ULL, 0xdbb25278ea9957ddULL, 0x4963113e05af023aULL, 0x38d3d003a0bd3167ULL,
      0x88f5d2f8427d15caULL, 0xf94513c5e76f2697ULL, 0x6b94508308597370ULL, 0x1a2491bead4b402dULL,
      0x7aeff05d8ea24bd5ULL, 0x0b5f31602bb07888ULL, 0x998e7226c4862d6fULL, 0xe83eb31b61941e32ULL,
      0xcd1b51825991f75eULL, 0xbcab90bffc83c403ULL, 0x2e7ad3f913b591e4ULL, 0x5fca12c4b6a7a2b9ULL,
      0x3f017327954ea941ULL, 0x4eb1b21a305c9a1cULL, 0xdc60f15cdf6acffbULL, 0xadd030617a78fca6ULL,
      0x1df6329a98b8d80bULL, 0x6c46f3a73daaeb56ULL, 0xfe97b0e1d29cbeb1ULL, 0x8f2771dc778e8decULL,
      0xefec103f54678614ULL, 0x9e5cd102f175b549ULL, 0x0c8d92441e43e0aeULL, 0x7d3d5379bb51d3f3ULL,

      0x0000000000000000ULL, 0xbfdb6c480f15915eULL, 0x4b6ffec346bcb1d7ULL, 0xf4b4928b49a92089ULL,
      0x96dffd868d7963aeULL, 0x290491ce826cf2f0ULL, 0xddb00345cbc5d279ULL, 0x626b6f0dc4d04327ULL,
      0x1966dd5e42655437ULL, 0xa6bdb1164d70c569ULL, 0x5209239d04d9e5e0ULL, 0xedd24fd50bcc74beULL,
      0x8fb920d8cf1c3799ULL, 0x30624c90c009a6c7ULL, 0xc4d6de1b89a0864eULL, 0x7b0db25386b51710ULL,
      0x32cdbabc84caa86eULL, 0x8d16d6f48bdf3930ULL, 0x79a2447fc27619b9ULL, 0xc6792837cd6388e7ULL,
      0xa412473a09b3cbc0ULL, 0x1bc92b7206a65a9eULL, 0xef7db9f94f0f7a17ULL, 0x50a6d5b1401aeb49ULL,
      0x2bab67e2c6affc59ULL, 0x94700baac9ba6d07ULL, 0x60c4992180134d8eULL, 0xdf1ff5698f06dcd0ULL,
      0xbd749a644bd69ff7ULL, 0x02aff62c44c30ea9ULL, 0xf61b64a70d6a2e20ULL, 0x49c008ef027fbf7eULL,
      0x659b7579099550dcULL, 0xda4019310680c182ULL, 0x2ef48bba4f29e10bULL, 0x912fe7f2403c7055ULL,
      0xf34488ff84ec3372ULL, 0x4c9fe4b78bf9a22cULL, 0xb82b763cc25082a5ULL, 0x07f01a74cd4513fbULL,
      0x7cfda8274bf004ebULL, 0xc326c46f44e595b5ULL, 0x379256e40d4cb53cULL, 0x88493aac02592462ULL,
      0xea2255a1c6896745ULL, 0x55f939e9c99cf61bULL, 0xa14dab628035d692ULL, 0x1e96c72a8f2047ccULL,
      0x5756cfc58d5ff8b2ULL, 0xe88da38d824a69ecULL, 0x1c393106cbe34965ULL, 0xa3e25d4ec4f6d83bULL,
      0xc189324300269b1cULL, 0x7e525e0b0f330a42ULL, 0x8ae6cc80469a2acbULL, 0x353da0c8498fbb95ULL,
      0x4e30129bcf3aac85ULL, 0xf1eb7ed3c02f3ddbULL, 0x055fec5889861d52ULL, 0xba84801086938c0cULL,
      0xd8efef1d4243cf2bULL, 0x673483554d565e75ULL, 0x938011de04ff7efcULL, 0x2c5b7d960beaefa2ULL,
      0xcb36eaf2132aa1b8ULL, 0x74ed86ba1c3f30e6ULL, 0x805914315596106fULL, 0x3f8278795a838131ULL,
      0x5de917749e53c216ULL, 0xe2327b3c91465348ULL, 0x1686e9b7d8ef73c1ULL, 0xa95d85ffd7fae29fULL,
      0xd25037ac514ff58fULL, 0x6d8b5be45e5a64d1ULL, 0x993fc96f17f34458ULL, 0x26e4a52718e6d506ULL,
      0x448fca2adc369621ULL, 0xfb54a662d323077fULL, 0x0fe034e99a8a27f6ULL, 0xb03b58a1959fb6a8ULL,
      0xf9fb504e97e009d6ULL, 0x46203c0698f59888ULL, 0xb294ae8dd15cb801ULL, 0x0d4fc2c5de49295fULL,
      0x6f24adc81a996a78ULL, 0xd0ffc180158cfb26ULL, 0x244b530b5c25dbafULL, 0x9b903f4353304af1ULL,
      0xe09d8d10d5855de1ULL, 0x5f46e158da90ccbfULL, 0xabf273d39339ec36ULL, 0x14291f9b9c2c7d68ULL,
      0x7642709658fc3e4fULL, 0xc9991cde57e9af11ULL, 0x3d2d8e551e408f98ULL, 0x82f6e21d11551ec6ULL,
      0xaead9f8b1abff164ULL, 0x1176f3c315aa603aULL, 0xe5c261485c0340b3ULL, 0x5a190d005316d1edULL,
      0x3872620d97c692caULL, 0x87a90e4598d30394ULL, 0x731d9cced17a231dULL, 0xccc6f086de6fb243ULL,
      0xb7cb42d558daa553ULL, 0x08102e9d57cf340dULL, 0xfca4bc161e661484ULL, 0x437fd05e117385daULL,
      0x2114bf53d5a3c6fdULL, 0x9ecfd31bdab657a3ULL, 0x6a7b4190931f772aULL, 0xd5a02dd89c0ae674ULL,
      0x9c6025379e75590aULL, 0x23bb497f9160c854ULL, 0xd70fdbf4d8c9e8ddULL, 0x68d4b7bcd7dc7983ULL,
      0x0abfd8b1130c3aa4ULL, 0xb564b4f91c19abfaULL, 0x41d0267255b08b73ULL, 0xfe0b4a3a5aa51a2dULL,
      0x8506f869dc100d3dULL, 0x3add9421d3059c63ULL, 0xce6906aa9aacbceaULL, 0x71b26ae295b92db4ULL,
      0x13d905ef51696e93ULL, 0xac0269a75e7cffcdULL, 0x58b6fb2c17d5df44ULL, 0xe76d976418c04e1aULL,
      0xa2b4f3b77ec2d01bULL, 0x1d6f9fff71d74145ULL, 0xe9db0d74387e61ccULL, 0x5600613c376bf092ULL,
      0x346b0e31f3bbb3b5ULL, 0x8bb06279fcae22ebULL, 0x7f04f0f2b5070262ULL, 0xc0df9cbaba12933cULL,
      0xbbd22ee93ca7842cULL, 0x040942a133b21572ULL, 0xf0bdd02a7a1b35fbULL, 0x4f66bc62750ea4a5ULL,
      0x2d0dd36fb1dee782ULL, 0x92d6bf27becb76dcULL, 0x66622dacf7625655ULL, 0xd9b941e4f877c70bULL,
      0x9079490bfa087875ULL, 0x2fa22543f51de92bULL, 0xdb16b7c8bcb4c9a2ULL, 0x64cddb80b3a158fcULL,
      0x06a6b48d77711bdbULL, 0xb97dd8c578648a85ULL, 0x4dc94a4e31cdaa0cULL, 0xf21226063ed83b52ULL,
      0x891f9455b86d2c42ULL, 0x36c4f81db778bd1cULL, 0xc2706a96fed19d95ULL, 0x7dab06def1c40ccbULL,
      0x1fc069d335144fecULL, 0xa01b059b3a01deb2ULL, 0x54af971073a8fe3bULL, 0xeb74fb587cbd6f65ULL,
      0xc72f86ce775780c7ULL, 0x78f4ea8678421199ULL, 0x8c40780d31eb3110ULL, 0x339b14453efea04eULL,
      0x51f07b48fa2ee369ULL, 0xee2b1700f53b7237ULL, 0x1a9f858bbc9252beULL, 0xa544e9c3b387c3e0ULL,
      0xde495b903532d4f0ULL, 0x619237d83a2745aeULL, 0x9526a553738e6527ULL, 0x2afdc91b7c9bf479ULL,
      0x4896a616b84bb75eULL, 0xf74dca5eb75e2600ULL, 0x03f958d5fef70689ULL, 0xbc22349df1e297d7ULL,
      0xf5e23c72f39d28a9ULL, 0x4a39503afc88b9f7ULL, 0xbe8dc2b1b521997eULL, 0x0156aef9ba340820ULL,
      0x633dc1f47ee44b07ULL, 0xdce6adbc71f1da59ULL, 0x28523f373858fad0ULL, 0x9789537f374d6b8eULL,
      0xec84e12cb1f87c9eULL, 0x535f8d64beededc0ULL, 0xa7eb1feff744cd49ULL, 0x183073a7f8515c17ULL,
      0x7a5b1caa3c811f30ULL, 0xc58070e233948e6eULL, 0x3134e2697a3daee7ULL, 0x8eef8e2175283fb9ULL,
      0x698219456de871a3ULL, 0xd659750d62fde0fdULL, 0x22ede7862b54c074ULL, 0x9d368bce2441512aULL,
      0xff5de4c3e091120dULL, 0x4086888bef848353ULL, 0xb4321a00a62da3daULL, 0x0be97648a9383284ULL,
      0x70e4c41b2f8d2594ULL, 0xcf3fa8532098b4caULL, 0x3b8b3ad869319443ULL, 0x845056906624051dULL,
      0xe63b399da2f4463aULL, 0x59e055d5ade1d764ULL, 0xad54c75ee448f7edULL, 0x128fab16eb5d66b3ULL,
      0x5b4fa3f9e922d9cdULL, 0xe494cfb1e6374893ULL, 0x10205d3aaf9e681aULL, 0xaffb3172a08bf944ULL,
      0xcd905e7f645bba63ULL, 0x724b32376b4e2b3dULL, 0x86ffa0bc22e70bb4ULL, 0x3924ccf42df29aeaULL,
      0x42297ea7ab478dfaULL, 0xfdf212efa4521ca4ULL, 0x09468064edfb3c2dULL, 0xb69dec2ce2eead73ULL,
      0xd4f68321263eee54ULL, 0x6b2def69292b7f0aULL, 0x9f997de260825f83ULL, 0x204211aa6f97ceddULL,
      0x0c196c3c647d217fULL, 0xb3c200746b68b021ULL, 0x477692ff22c190a8ULL, 0xf8adfeb72dd401f6ULL,
      0x9ac691bae90442d1ULL, 0x251dfdf2e611d38fULL, 0xd1a96f79afb8f306ULL, 0x6e720331a0ad6258ULL,
      0x157fb16226187548ULL, 0xaaa4dd2a290de416ULL, 0x5e104fa160a4c49fULL, 0xe1cb23e96fb155c1ULL,
      0x83a04ce4ab6116e6ULL, 0x3c7b20aca47487b8ULL, 0xc8cfb227eddda731ULL, 0x7714de6fe2c8366fULL,
      0x3ed4d680e0b78911ULL, 0x810fbac8efa2184fULL, 0x75bb2843a60b38c6ULL, 0xca60440ba91ea998ULL,
      0xa80b2b066dceeabfULL, 0x17d0474e62db7be1ULL, 0xe364d5c52b725b68ULL, 0x5cbfb98d2467ca36ULL,
      0x27b20bdea2d2dd26ULL, 0x98696796adc74c78ULL, 0x6cddf51de46e6cf1ULL, 0xd3069955eb7bfdafULL,
      0xb16df6582fabbe88ULL, 0x0eb69a1020be2fd6ULL, 0xfa02089b69170f5fULL, 0x45d964d366029e01ULL,

      0x0000000000000000ULL, 0x3ea616bd2ae10d77ULL, 0x7d4c2d7a55c21aeeULL, 0x43ea3bc77f231799ULL,
      0xfa985af4ab8435dcULL, 0xc43e4c49816538abULL, 0x87d4778efe462f32ULL, 0xb9726133d4a72245ULL,
      0xc1e993ba0f9ff8d3ULL, 0xff4f8507257ef5a4ULL, 0xbca5bec05a5de23dULL, 0x8203a87d70bcef4aULL,
      0x3b71c94ea41bcd0fULL, 0x05d7dff38efac078ULL, 0x463de434f1d9d7e1ULL, 0x789bf289db38da96ULL,
      0xb70a012747a862cdULL, 0x89ac179a6d496fbaULL, 0xca462c5d126a7823ULL, 0xf4e03ae0388b7554ULL,
      0x4d925bd3ec2c5711ULL, 0x73344d6ec6cd5a66ULL, 0x30de76a9b9ee4dffULL, 0x0e786014930f4088ULL,
      0x76e3929d48379a1eULL, 0x4845842062d69769ULL, 0x0bafbfe71df580f0ULL, 0x3509a95a37148d87ULL,
      0x8c7bc869e3b3afc2ULL, 0xb2ddded4c952a2b5ULL, 0xf137e513b671b52cULL, 0xcf91f3ae9c90b85bULL,
      0x5acd241dd7c756f1ULL, 0x646b32a0fd265b86ULL, 0x2781096782054c1fULL, 0x19271fdaa8e44168ULL,
      0xa0557ee97c43632dULL, 0x9ef3685456a26e5aULL, 0xdd195393298179c3ULL, 0xe3bf452e036074b4ULL,
      0x9b24b7a7d858ae22ULL, 0xa582a11af2b9a355ULL, 0xe6689add8d9ab4ccULL, 0xd8ce8c60a77bb9bbULL,
      0x61bced5373dc9bfeULL, 0x5f1afbee593d9689ULL, 0x1cf0c029261e8110ULL, 0x2256d6940cff8c67ULL,
      0xedc7253a906f343cULL, 0xd3613387ba8e394bULL, 0x908b0840c5ad2ed2ULL, 0xae2d1efdef4c23a5ULL,
      0x175f7fce3beb01e0ULL, 0x29f96973110a0c97ULL, 0x6a1352b46e291b0eULL, 0x54b5440944c81679ULL,
      0x2c2eb6809ff0ccefULL, 0x1288a03db511c198ULL, 0x51629bfaca32d601ULL, 0x6fc48d47e0d3db76ULL,
      0xd6b6ec743474f933ULL, 0xe810fac91e95f444ULL, 0xabfac10e61b6e3ddULL, 0x955cd7b34b57eeaaULL,
      0xb59a483baf8eade2ULL, 0x8b3c5e86856fa095ULL, 0xc8d66541fa4cb70cULL, 0xf67073fcd0adba7bULL,
      0x4f0212cf040a983eULL, 0x71a404722eeb9549ULL, 0x324e3fb551c882d0ULL, 0x0ce829087b298fa7ULL,
      0x7473db81a0115531ULL, 0x4ad5cd3c8af05846ULL, 0x093ff6fbf5d34fdfULL, 0x3799e046df3242a8ULL,
      0x8eeb81750b9560edULL, 0xb04d97c821746d9aULL, 0xf3a7ac0f5e577a03ULL, 0xcd01bab274b67774ULL,
      0x0290491ce826cf2fULL, 0x3c365fa1c2c7c258ULL, 0x7fdc6466bde4d5c1ULL, 0x417a72db9705d8b6ULL,
      0xf80813e843a2faf3ULL, 0xc6ae05556943f784ULL, 0x85443e921660e01dULL, 0xbbe2282f3c81ed6aULL,
      0xc379daa6e7b937fcULL, 0xfddfcc1bcd583a8bULL, 0xbe35f7dcb27b2d12ULL, 0x8093e161989a2065ULL,
      0x39e180524c3d0220ULL, 0x074796ef66dc0f57ULL, 0x44adad2819ff18ceULL, 0x7a0bbb95331e15b9ULL,
      0xef576c267849fb13ULL, 0xd1f17a9b52a8f664ULL, 0x921b415c2d8be1fdULL, 0xacbd57e1076aec8aULL,
      0x15cf36d2d3cdcecfULL, 0x2b69206ff92cc3b8ULL, 0x68831ba8860fd421ULL, 0x56250d15aceed956ULL,
      0x2ebeff9c77d603c0ULL, 0x1018e9215d370eb7ULL, 0x53f2d2e62214192eULL, 0x6d54c45b08f51459ULL,
      0xd426a568dc52361cULL, 0xea80b3d5f6b33b6bULL, 0xa96a881289902cf2ULL, 0x97cc9eafa3712185ULL,
      0x585d6d013fe199deULL, 0x66fb7bbc150094a9ULL, 0x2511407b6a238330ULL, 0x1bb756c640c28e47ULL,
      0xa2c537f59465ac02ULL, 0x9c632148be84a175ULL, 0xdf891a8fc1a7b6ecULL, 0xe12f0c32eb46bb9bULL,
      0x99b4febb307e610dULL, 0xa712e8061a9f6c7aULL, 0xe4f8d3c165bc7be3ULL, 0xda5ec57c4f5d7694ULL,
      0x632ca44f9bfa54d1ULL, 0x5d8ab2f2b11b59a6ULL, 0x1e608935ce384e3fULL, 0x20c69f88e4d94348ULL,
      0x5fedb624078ac8afULL, 0x614ba0992d6bc5d8ULL, 0x22a19b5e5248d241ULL, 0x1c078de378a9df36ULL,
      0xa575ecd0ac0efd73ULL, 0x9bd3fa6d86eff004ULL, 0xd839c1aaf9cce79dULL, 0xe69fd717d32deaeaULL,
      0x9e04259e0815307cULL, 0xa0a2332322f43d0bULL, 0xe34808e45dd72a92ULL, 0xddee1e59773627e5ULL,
      0x649c7f6aa39105a0ULL, 0x5a3a69d7897008d7ULL, 0x19d05210f6531f4eULL, 0x277644addcb21239ULL,
      0xe8e7b7034022aa62ULL, 0xd641a1be6ac3a715ULL, 0x95ab9a7915e0b08cULL, 0xab0d8cc43f01bdfbULL,
      0x127fedf7eba69fbeULL, 0x2cd9fb4ac14792c9ULL, 0x6f33c08dbe648550ULL, 0x5195d63094858827ULL,
      0x290e24b94fbd52b1ULL, 0x17a83204655c5fc6ULL, 0x544209c31a7f485fULL, 0x6ae41f7e309e4528ULL,
      0xd3967e4de439676dULL, 0xed3068f0ced86a1aULL, 0xaeda5337b1fb7d83ULL, 0x907c458a9b1a70f4ULL,
      0x05209239d04d9e5eULL, 0x3b868484faac9329ULL, 0x786cbf43858f84b0ULL, 0x46caa9feaf6e89c7ULL,
      0xffb8c8cd7bc9ab82ULL, 0xc11ede705128a6f5ULL, 0x82f4e5b72e0bb16cULL, 0xbc52f30a04eabc1bULL,
      0xc4c90183dfd2668dULL, 0xfa6f173ef5336bfaULL, 0xb9852cf98a107c63ULL, 0x87233a44a0f17114ULL,
      0x3e515b7774565351ULL, 0x00f74dca5eb75e26ULL, 0x431d760d219449bfULL, 0x7dbb60b00b7544c8ULL,
      0xb22a931e97e5fc93ULL, 0x8c8c85a3bd04f1e4ULL, 0xcf66be64c227e67dULL, 0xf1c0a8d9e8c6eb0aULL,
      0x48b2c9ea3c61c94fULL, 0x7614df571680c438ULL, 0x35fee49069a3d3a1ULL, 0x0b58f22d4342ded6ULL,
      0x73c300a4987a0440ULL, 0x4d651619b29b0937ULL, 0x0e8f2ddecdb81eaeULL, 0x30293b63e75913d9ULL,
      0x895b5a5033fe319cULL, 0xb7fd4ced191f3cebULL, 0xf417772a663c2b72ULL, 0xcab161974cdd2605ULL,
      0xea77fe1fa804654dULL, 0xd4d1e8a282e5683aULL, 0x973bd365fdc67fa3ULL, 0xa99dc5d8d72772d4ULL,
      0x10efa4eb03805091ULL, 0x2e49b25629615de6ULL, 0x6da3899156424a7fULL, 0x53059f2c7ca34708ULL,
      0x2b9e6da5a79b9d9eULL, 0x15387b188d7a90e9ULL, 0x56d240dff2598770ULL, 0x68745662d8b88a07ULL,
      0xd10637510c1fa842ULL, 0xefa021ec26fea535ULL, 0xac4a1a2b59ddb2acULL, 0x92ec0c96733cbfdbULL,
      0x5d7dff38efac0780ULL, 0x63dbe985c54d0af7ULL, 0x2031d242ba6e1d6eULL, 0x1e97c4ff908f1019ULL,
      0xa7e5a5cc4428325cULL, 0x9943b3716ec93f2bULL, 0xdaa988b611ea28b2ULL, 0xe40f9e0b3b0b25c5ULL,
      0x9c946c82e033ff53ULL, 0xa2327a3fcad2f224ULL, 0xe1d841f8b5f1e5bdULL, 0xdf7e57459f10e8caULL,
      0x660c36764bb7ca8fULL, 0x58aa20cb6156c7f8ULL, 0x1b401b0c1e75d061ULL, 0x25e60db13494dd16ULL,
      0xb0bada027fc333bcULL, 0x8e1cccbf55223ecbULL, 0xcdf6f7782a012952ULL, 0xf350e1c500e02425ULL,
      0x4a2280f6d4470660ULL, 0x7484964bfea60b17ULL, 0x376ead8c81851c8eULL, 0x09c8bb31ab6411f9ULL,
      0x715349b8705ccb6fULL, 0x4ff55f055abdc618ULL, 0x0c1f64c2259ed181ULL, 0x32b9727f0f7fdcf6ULL,
      0x8bcb134cdbd8feb3ULL, 0xb56d05f1f139f3c4ULL, 0xf6873e368e1ae45dULL, 0xc821288ba4fbe92aULL,
      0x07b0db25386b5171ULL, 0x3916cd98128a5c06ULL, 0x7afcf65f6da94b9fULL, 0x445ae0e2474846e8ULL,
      0xfd2881d193ef64adULL, 0xc38e976cb90e69daULL, 0x8064acabc62d7e43ULL, 0xbec2ba16eccc7334ULL,
      0xc659489f37f4a9a2ULL, 0xf8ff5e221d15a4d5ULL, 0xbb1565e56236b34cULL, 0x85b3735848d7be3bULL,
      0x3cc1126b9c709c7eULL, 0x026704d6b6919109ULL, 0x418d3f11c9b28690ULL, 0x7f2b29ace3538be7ULL,

      0x0000000000000000ULL, 0x169489cc969951e5ULL, 0x2d2913992d32a3caULL, 0x3bbd9a55bbabf22fULL,
      0x5a5227325a654794ULL, 0x4cc6aefeccfc1671ULL, 0x777b34ab7757e45eULL, 0x61efbd67e1ceb5bbULL,
      0xb4a44e64b4ca8f28ULL, 0xa230c7a82253decdULL, 0x998d5dfd99f82ce2ULL, 0x8f19d4310f617d07ULL,
      0xeef66956eeafc8bcULL, 0xf862e09a78369959ULL, 0xc3df7acfc39d6b76ULL, 0xd54bf30355043a93ULL,
      0x5d91ba9a31028d3bULL, 0x4b053356a79bdcdeULL, 0x70b8a9031c302ef1ULL, 0x662c20cf8aa97f14ULL,
      0x07c39da86b67caafULL, 0x11571464fdfe9b4aULL, 0x2aea8e3146556965ULL, 0x3c7e07fdd0cc3880ULL,
      0xe935f4fe85c80213ULL, 0xffa17d32135153f6ULL, 0xc41ce767a8faa1d9ULL, 0xd2886eab3e63f03cULL,
      0xb367d3ccdfad4587ULL, 0xa5f35a0049341462ULL, 0x9e4ec055f29fe64dULL, 0x88da49996406b7a8ULL,
      0xbb23753462051a76ULL, 0xadb7fcf8f49c4b93ULL, 0x960a66ad4f37b9bcULL, 0x809eef61d9aee859ULL,
      0xe171520638605de2ULL, 0xf7e5dbcaaef90c07ULL, 0xcc58419f1552fe28ULL, 0xdaccc85383cbafcdULL,
      0x0f873b50d6cf955eULL, 0x1913b29c4056c4bbULL, 0x22ae28c9fbfd3694ULL, 0x343aa1056d646771ULL,
      0x55d51c628caad2caULL, 0x434195ae1a33832fULL, 0x78fc0ffba1987100ULL, 0x6e688637370120e5ULL,
      0xe6b2cfae5307974dULL, 0xf0264662c59ec6a8ULL, 0xcb9bdc377e353487ULL, 0xdd0f55fbe8ac6562ULL,
      0xbce0e89c0962d0d9ULL, 0xaa7461509ffb813cULL, 0x91c9fb0524507313ULL, 0x875d72c9b2c922f6ULL,
      0x521681cae7cd1865ULL, 0x4482080671544980ULL, 0x7f3f9253caffbbafULL, 0x69ab1b9f5c66ea4aULL,
      0x0844a6f8bda85ff1ULL, 0x1ed02f342b310e14ULL, 0x256db561909afc3bULL, 0x33f93cad0603addeULL,
      0x429fcc3b9c9da787ULL, 0x540b45f70a04f662ULL, 0x6fb6dfa2b1af044dULL, 0x7922566e273655a8ULL,
      0x18cdeb09c6f8e013ULL, 0x0e5962c55061b1f6ULL, 0x35e4f890ebca43d9ULL, 0x2370715c7d53123cULL,
      0xf63b825f285728afULL, 0xe0af0b93bece794aULL, 0xdb1291c605658b65ULL, 0xcd86180a93fcda80ULL,
      0xac69a56d72326f3bULL, 0xbafd2ca1e4ab3edeULL, 0x8140b6f45f00ccf1ULL, 0x97d43f38c9999d14ULL,
      0x1f0e76a1ad9f2abcULL, 0x099aff6d3b067b59ULL, 0x3227653880ad8976ULL, 0x24b3ecf41634d893ULL,
      0x455c5193f7fa6d28ULL, 0x53c8d85f61633ccdULL, 0x6875420adac8cee2ULL, 0x7ee1cbc64c519f07ULL,
      0xabaa38c51955a594ULL, 0xbd3eb1098fccf471ULL, 0x86832b5c3467065eULL, 0x9017a290a2fe57bbULL,
      0xf1f81ff74330e200ULL, 0xe76c963bd5a9b3e5ULL, 0xdcd10c6e6e0241caULL, 0xca4585a2f89b102fULL,
      0xf9bcb90ffe98bdf1ULL, 0xef2830c36801ec14ULL, 0xd495aa96d3aa1e3bULL, 0xc201235a45334fdeULL,
      0xa3ee9e3da4fdfa65ULL, 0xb57a17f13264ab80ULL, 0x8ec78da489cf59afULL, 0x985304681f56084aULL,
      0x4d18f76b4a5232d9ULL, 0x5b8c7ea7dccb633cULL, 0x6031e4f267609113ULL, 0x76a56d3ef1f9c0f6ULL,
      0x174ad0591037754dULL, 0x01de599586ae24a8ULL, 0x3a63c3c03d05d687ULL, 0x2cf74a0cab9c8762ULL,
      0xa42d0395cf9a30caULL, 0xb2b98a595903612fULL, 0x8904100ce2a89300ULL, 0x9f9099c07431c2e5ULL,
      0xfe7f24a795ff775eULL, 0xe8ebad6b036626bbULL, 0xd356373eb8cdd494ULL, 0xc5c2bef22e548571ULL,
      0x10894df17b50bfe2ULL, 0x061dc43dedc9ee07ULL, 0x3da05e6856621c28ULL, 0x2b34d7a4c0fb4dcdULL,
      0x4adb6ac32135f876ULL, 0x5c4fe30fb7aca993ULL, 0x67f2795a0c075bbcULL, 0x7166f0969a9e0a59ULL,
      0x853f9877393b4f0eULL, 0x93ab11bbafa21eebULL, 0xa8168bee1409ecc4ULL, 0xbe8202228290bd21ULL,
      0xdf6dbf45635e089aULL, 0xc9f93689f5c7597fULL, 0xf244acdc4e6cab50ULL, 0xe4d02510d8f5fab5ULL,
      0x319bd6138df1c026ULL, 0x270f5fdf1b6891c3ULL, 0x1cb2c58aa0c363ecULL, 0x0a264c46365a3209ULL,
      0x6bc9f121d79487b2ULL, 0x7d5d78ed410dd657ULL, 0x46e0e2b8faa62478ULL, 0x50746b746c3f759dULL,
      0xd8ae22ed0839c235ULL, 0xce3aab219ea093d0ULL, 0xf5873174250b61ffULL, 0xe313b8b8b392301aULL,
      0x82fc05df525c85a1ULL, 0x94688c13c4c5d444ULL, 0xafd516467f6e266bULL, 0xb9419f8ae9f7778eULL,
      0x6c0a6c89bcf34d1dULL, 0x7a9ee5452a6a1cf8ULL, 0x41237f1091c1eed7ULL, 0x57b7f6dc0758bf32ULL,
      0x36584bbbe6960a89ULL, 0x20ccc277700f5b6cULL, 0x1b715822cba4a943ULL, 0x0de5d1ee5d3df8a6ULL,
      0x3e1ced435b3e5578ULL, 0x2888648fcda7049dULL, 0x1335feda760cf6b2ULL, 0x05a17716e095a757ULL,
      0x644eca71015b12ecULL, 0x72da43bd97c24309ULL, 0x4967d9e82c69b126ULL, 0x5ff35024baf0e0c3ULL,
      0x8ab8a327eff4da50ULL, 0x9c2c2aeb796d8bb5ULL, 0xa791b0bec2c6799aULL, 0xb1053972545f287fULL,
      0xd0ea8415b5919dc4ULL, 0xc67e0dd92308cc21ULL, 0xfdc3978c98a33e0eULL, 0xeb571e400e3a6febULL,
      0x638d57d96a3cd843ULL, 0x7519de15fca589a6ULL, 0x4ea44440470e7b89ULL, 0x5830cd8cd1972a6cULL,
      0x39df70eb30599fd7ULL, 0x2f4bf927a6c0ce32ULL, 0x14f663721d6b3c1dULL, 0x0262eabe8bf26df8ULL,
      0xd72919bddef6576bULL, 0xc1bd9071486f068eULL, 0xfa000a24f3c4f4a1ULL, 0xec9483e8655da544ULL,
      0x8d7b3e8f849310ffULL, 0x9befb743120a411aULL, 0xa0522d16a9a1b335ULL, 0xb6c6a4da3f38e2d0ULL,
      0xc7a0544ca5a6e889ULL, 0xd134dd80333fb96cULL, 0xea8947d588944b43ULL, 0xfc1dce191e0d1aa6ULL,
      0x9df2737effc3af1dULL, 0x8b66fab2695afef8ULL, 0xb0db60e7d2f10cd7ULL, 0xa64fe92b44685d32ULL,
      0x73041a28116c67a1ULL, 0x659093e487f53644ULL, 0x5e2d09b13c5ec46bULL, 0x48b9807daac7958eULL,
      0x29563d1a4b092035ULL, 0x3fc2b4d6dd9071d0ULL, 0x047f2e83663b83ffULL, 0x12eba74ff0a2d21aULL,
      0x9a31eed694a465b2ULL, 0x8ca5671a023d3457ULL, 0xb718fd4fb996c678ULL, 0xa18c74832f0f979dULL,
      0xc063c9e4cec12226ULL, 0xd6f74028585873c3ULL, 0xed4ada7de3f381ecULL, 0xfbde53b1756ad009ULL,
      0x2e95a0b2206eea9aULL, 0x3801297eb6f7bb7fULL, 0x03bcb32b0d5c4950ULL, 0x15283ae79bc518b5ULL,
      0x74c787807a0bad0eULL, 0x62530e4cec92fcebULL, 0x59ee941957390ec4ULL, 0x4f7a1dd5c1a05f21ULL,
      0x7c832178c7a3f2ffULL, 0x6a17a8b4513aa31aULL, 0x51aa32e1ea915135ULL, 0x473ebb2d7c0800d0ULL,
      0x26d1064a9dc6b56bULL, 0x30458f860b5fe48eULL, 0x0bf815d3b0f416a1ULL, 0x1d6c9c1f266d4744ULL,
      0xc8276f1c73697dd7ULL, 0xdeb3e6d0e5f02c32ULL, 0xe50e7c855e5bde1dULL, 0xf39af549c8c28ff8ULL,
      0x9275482e290c3a43ULL, 0x84e1c1e2bf956ba6ULL, 0xbf5c5bb7043e9989ULL, 0xa9c8d27b92a7c86cULL,
      0x21129be2f6a17fc4ULL, 0x3786122e60382e21ULL, 0x0c3b887bdb93dc0eULL, 0x1aaf01b74d0a8debULL,
      0x7b40bcd0acc43850ULL, 0x6dd4351c3a5d69b5ULL, 0x5669af4981f69b9aULL, 0x40fd2685176fca7fULL,
      0x95b6d586426bf0ecULL, 0x83225c4ad4f2a109ULL, 0xb89fc61f6f595326ULL, 0xae0b4fd3f9c002c3ULL,
      0xcfe4f2b4180eb778ULL, 0xd9707b788e97e69dULL, 0xe2cde12d353c14b2ULL, 0xf45968e1a3a54557ULL,

      0x0000000000000000ULL, 0x0aed36d1a3bb9d7fULL, 0x15da6da347773afeULL, 0x1f375b72e4cca781ULL,
      0x2bb4db468eee75fcULL, 0x2159ed972d55e883ULL, 0x3e6eb6e5c9994f02ULL, 0x348380346a22d27dULL,
      0x5769b68d1ddcebf8ULL, 0x5d84805cbe677687ULL, 0x42b3db2e5aabd106ULL, 0x485eedfff9104c79ULL,
      0x7cdd6dcb93329e04ULL, 0x76305b1a3089037bULL, 0x69070068d445a4faULL, 0x63ea36b977fe3985ULL,
      0xaed36d1a3bb9d7f0ULL, 0xa43e5bcb98024a8fULL, 0xbb0900b97cceed0eULL, 0xb1e43668df757071ULL,
      0x8567b65cb557a20cULL, 0x8f8a808d16ec3f73ULL, 0x90bddbfff22098f2ULL, 0x9a50ed2e519b058dULL,
      0xf9badb9726653c08ULL, 0xf357ed4685dea177ULL, 0xec60b634611206f6ULL, 0xe68d80e5c2a99b89ULL,
      0xd20e00d1a88b49f4ULL, 0xd8e336000b30d48bULL, 0xc7d46d72effc730aULL, 0xcd395ba34c47ee75ULL,
      0x697ffc672fe43c8bULL, 0x6392cab68c5fa1f4ULL, 0x7ca591c468930675ULL, 0x7648a715cb289b0aULL,
      0x42cb2721a10a4977ULL, 0x482611f002b1d408ULL, 0x57114a82e67d7389ULL, 0x5dfc7c5345c6eef6ULL,
      0x3e164aea3238d773ULL, 0x34fb7c3b91834a0cULL, 0x2bcc2749754fed8dULL, 0x21211198d6f470f2ULL,
      0x15a291acbcd6a28fULL, 0x1f4fa77d1f6d3ff0ULL, 0x0078fc0ffba19871ULL, 0x0a95cade581a050eULL,
      0xc7ac917d145deb7bULL, 0xcd41a7acb7e67604ULL, 0xd276fcde532ad185ULL, 0xd89bca0ff0914cfaULL,
      0xec184a3b9ab39e87ULL, 0xe6f57cea390803f8ULL, 0xf9c22798ddc4a479ULL, 0xf32f11497e7f3906ULL,
      0x90c527f009810083ULL, 0x9a281121aa3a9dfcULL, 0x851f4a534ef63a7dULL, 0x8ff27c82ed4da702ULL,
      0xbb71fcb6876f757fULL, 0xb19cca6724d4e800ULL, 0xaeab9115c0184f81ULL, 0xa446a7c463a3d2feULL,
      0xd2fff8ce5fc87916ULL, 0xd812ce1ffc73e469ULL, 0xc725956d18bf43e8ULL, 0xcdc8a3bcbb04de97ULL,
      0xf94b2388d1260ceaULL, 0xf3a61559729d9195ULL, 0xec914e2b96513614ULL, 0xe67c78fa35eaab6bULL,
      0x85964e43421492eeULL, 0x8f7b7892e1af0f91ULL, 0x904c23e00563a810ULL, 0x9aa11531a6d8356fULL,
      0xae229505ccfae712ULL, 0xa4cfa3d46f417a6dULL, 0xbbf8f8a68b8dddecULL, 0xb115ce7728364093ULL,
      0x7c2c95d46471aee6ULL, 0x76c1a305c7ca3399ULL, 0x69f6f87723069418ULL, 0x631bcea680bd0967ULL,
      0x57984e92ea9fdb1aULL, 0x5d75784349244665ULL, 0x42422331ade8e1e4ULL, 0x48af15e00e537c9bULL,
      0x2b45235979ad451eULL, 0x21a81588da16d861ULL, 0x3e9f4efa3eda7fe0ULL, 0x3472782b9d61e29fULL,
      0x00f1f81ff74330e2ULL, 0x0a1ccece54f8ad9dULL, 0x152b95bcb0340a1cULL, 0x1fc6a36d138f9763ULL,
      0xbb8004a9702c459dULL, 0xb16d3278d397d8e2ULL, 0xae5a690a375b7f63ULL, 0xa4b75fdb94e0e21cULL,
      0x9034dfeffec23061ULL, 0x9ad9e93e5d79ad1eULL, 0x85eeb24cb9b50a9fULL, 0x8f03849d1a0e97e0ULL,
      0xece9b2246df0ae65ULL, 0xe60484f5ce4b331aULL, 0xf933df872a87949bULL, 0xf3dee956893c09e4ULL,
      0xc75d6962e31edb99ULL, 0xcdb05fb340a546e6ULL, 0xd28704c1a469e167ULL, 0xd86a321007d27c18ULL,
      0x155369b34b95926dULL, 0x1fbe5f62e82e0f12ULL, 0x008904100ce2a893ULL, 0x0a6432c1af5935ecULL,
      0x3ee7b2f5c57be791ULL, 0x340a842466c07aeeULL, 0x2b3ddf56820cdd6fULL, 0x21d0e98721b74010ULL,
      0x423adf3e56497995ULL, 0x48d7e9eff5f2e4eaULL, 0x57e0b29d113e436bULL, 0x5d0d844cb285de14ULL,
      0x698e0478d8a70c69ULL, 0x636332a97b1c9116ULL, 0x7c5469db9fd03697ULL, 0x76b95f0a3c6babe8ULL,
      0x9126d7cfe7076147ULL, 0x9bcbe11e44bcfc38ULL, 0x84fcba6ca0705bb9ULL, 0x8e118cbd03cbc6c6ULL,
      0xba920c8969e914bbULL, 0xb07f3a58ca5289c4ULL, 0xaf48612a2e9e2e45ULL, 0xa5a557fb8d25b33aULL,
      0xc64f6142fadb8abfULL, 0xcca25793596017c0ULL, 0xd3950ce1bdacb041ULL, 0xd9783a301e172d3eULL,
      0xedfbba047435ff43ULL, 0xe7168cd5d78e623cULL, 0xf821d7a73342c5bdULL, 0xf2cce17690f958c2ULL,
      0x3ff5bad5dcbeb6b7ULL, 0x35188c047f052bc8ULL, 0x2a2fd7769bc98c49ULL, 0x20c2e1a738721136ULL,
      0x144161935250c34bULL, 0x1eac5742f1eb5e34ULL, 0x019b0c301527f9b5ULL, 0x0b763ae1b69c64caULL,
      0x689c0c58c1625d4fULL, 0x62713a8962d9c030ULL, 0x7d4661fb861567b1ULL, 0x77ab572a25aefaceULL,
      0x4328d71e4f8c28b3ULL, 0x49c5e1cfec37b5ccULL, 0x56f2babd08fb124dULL, 0x5c1f8c6cab408f32ULL,
      0xf8592ba8c8e35dccULL, 0xf2b41d796b58c0b3ULL, 0xed83460b8f946732ULL, 0xe76e70da2c2ffa4dULL,
      0xd3edf0ee460d2830ULL, 0xd900c63fe5b6b54fULL, 0xc6379d4d017a12ceULL, 0xccdaab9ca2c18fb1ULL,
      0xaf309d25d53fb634ULL, 0xa5ddabf476842b4bULL, 0xbaeaf08692488ccaULL, 0xb007c65731f311b5ULL,
      0x848446635bd1c3c8ULL, 0x8e6970b2f86a5eb7ULL, 0x915e2bc01ca6f936ULL, 0x9bb31d11bf1d6449ULL,
      0x568a46b2f35a8a3cULL, 0x5c67706350e11743ULL, 0x43502b11b42db0c2ULL, 0x49bd1dc017962dbdULL,
      0x7d3e9df47db4ffc0ULL, 0x77d3ab25de0f62bfULL, 0x68e4f0573ac3c53eULL, 0x6209c68699785841ULL,
      0x01e3f03fee8661c4ULL, 0x0b0ec6ee4d3dfcbbULL, 0x14399d9ca9f15b3aULL, 0x1ed4ab4d0a4ac645ULL,
      0x2a572b7960681438ULL, 0x20ba1da8c3d38947ULL, 0x3f8d46da271f2ec6ULL, 0x3560700b84a4b3b9ULL,
      0x43d92f01b8cf1851ULL, 0x493419d01b74852eULL, 0x560342a2ffb822afULL, 0x5cee74735c03bfd0ULL,
      0x686df44736216dadULL, 0x6280c296959af0d2ULL, 0x7db799e471565753ULL, 0x775aaf35d2edca2cULL,
      0x14b0998ca513f3a9ULL, 0x1e5daf5d06a86ed6ULL, 0x016af42fe264c957ULL, 0x0b87c2fe41df5428ULL,
      0x3f0442ca2bfd8655ULL, 0x35e9741b88461b2aULL, 0x2ade2f696c8abcabULL, 0x203319b8cf3121d4ULL,
      0xed0a421b8376cfa1ULL, 0xe7e774ca20cd52deULL, 0xf8d02fb8c401f55fULL, 0xf23d196967ba6820ULL,
      0xc6be995d0d98ba5dULL, 0xcc53af8cae232722ULL, 0xd364f4fe4aef80a3ULL, 0xd989c22fe9541ddcULL,
      0xba63f4969eaa2459ULL, 0xb08ec2473d11b926ULL, 0xafb99935d9dd1ea7ULL, 0xa554afe47a6683d8ULL,
      0x91d72fd0104451a5ULL, 0x9b3a1901b3ffccdaULL, 0x840d427357336b5bULL, 0x8ee074a2f488f624ULL,
      0x2aa6d366972b24daULL, 0x204be5b73490b9a5ULL, 0x3f7cbec5d05c1e24ULL, 0x3591881473e7835bULL,
      0x0112082019c55126ULL, 0x0bff3ef1ba7ecc59ULL, 0x14c865835eb26bd8ULL, 0x1e255352fd09f6a7ULL,
      0x7dcf65eb8af7cf22ULL, 0x7722533a294c525dULL, 0x68150848cd80f5dcULL, 0x62f83e996e3b68a3ULL,
      0x567bbead0419badeULL, 0x5c96887ca7a227a1ULL, 0x43a1d30e436e8020ULL, 0x494ce5dfe0d51d5fULL,
      0x8475be7cac92f32aULL, 0x8e9888ad0f296e55ULL, 0x91afd3dfebe5c9d4ULL, 0x9b42e50e485e54abULL,
      0xafc1653a227c86d6ULL, 0xa52c53eb81c71ba9ULL, 0xba1b0899650bbc28ULL, 0xb0f63e48c6b02157ULL,
      0xd31c08f1b14e18d2ULL, 0xd9f13e2012f585adULL, 0xc6c66552f639222cULL, 0xcc2b53835582bf53ULL,
      0xf8a8d3b73fa06d2eULL, 0xf245e5669c1bf051ULL, 0xed72be1478d757d0ULL, 0xe79f88c5db6ccaafULL,

      0x0000000000000000ULL, 0xb0bc2e589204f500ULL, 0x55a17ae27c9e796bULL, 0xe51d54baee9a8c6bULL,
      0xab42f5c4f93cf2d6ULL, 0x1bfedb9c6b3807d6ULL, 0xfee38f2685a28bbdULL, 0x4e5fa17e17a67ebdULL,
      0x625ccddaaaee76c7ULL, 0xd2e0e38238ea83c7ULL, 0x37fdb738d6700facULL, 0x874199604474faacULL,
      0xc91e381e53d28411ULL, 0x79a21646c1d67111ULL, 0x9cbf42fc2f4cfd7aULL, 0x2c036ca4bd48087aULL,
      0xc4b99bb555dced8eULL, 0x7405b5edc7d8188eULL, 0x9118e157294294e5ULL, 0x21a4cf0fbb4661e5ULL,
      0x6ffb6e71ace01f58ULL, 0xdf4740293ee4ea58ULL, 0x3a5a1493d07e6633ULL, 0x8ae63acb427a9333ULL,
      0xa6e5566fff329b49ULL, 0x165978376d366e49ULL, 0xf3442c8d83ace222ULL, 0x43f802d511a81722ULL,
      0x0da7a3ab060e699fULL, 0xbd1b8df3940a9c9fULL, 0x5806d9497a9010f4ULL, 0xe8baf711e894e5f4ULL,
      0xbdaa1139f32e4877ULL, 0x0d163f61612abd77ULL, 0xe80b6bdb8fb0311cULL, 0x58b745831db4c41cULL,
      0x16e8e4fd0a12baa1ULL, 0xa654caa598164fa1ULL, 0x43499e1f768cc3caULL, 0xf3f5b047e48836caULL,
      0xdff6dce359c03eb0ULL, 0x6f4af2bbcbc4cbb0ULL, 0x8a57a601255e47dbULL, 0x3aeb8859b75ab2dbULL,
      0x74b42927a0fccc66ULL, 0xc408077f32f83966ULL, 0x211553c5dc62b50dULL, 0x91a97d9d4e66400dULL,
      0x79138a8ca6f2a5f9ULL, 0xc9afa4d434f650f9ULL, 0x2cb2f06eda6cdc92ULL, 0x9c0ede3648682992ULL,
      0xd2517f485fce572fULL, 0x62ed5110cdcaa22fULL, 0x87f005aa23502e44ULL, 0x374c2bf2b154db44ULL,
      0x1b4f47560c1cd33eULL, 0xabf3690e9e18263eULL, 0x4eee3db47082aa55ULL, 0xfe5213ece2865f55ULL,
      0xb00db292f52021e8ULL, 0x00b19cca6724d4e8ULL, 0xe5acc87089be5883ULL, 0x5510e6281bbaad83ULL,
      0x4f8d0420becb0385ULL, 0xff312a782ccff685ULL, 0x1a2c7ec2c2557aeeULL, 0xaa90509a50518feeULL,
      0xe4cff1e447f7f153ULL, 0x5473dfbcd5f30453ULL, 0xb16e8b063b698838ULL, 0x01d2a55ea96d7d38ULL,
      0x2dd1c9fa14257542ULL, 0x9d6de7a286218042ULL, 0x7870b31868bb0c29ULL, 0xc8cc9d40fabff929ULL,
      0x86933c3eed198794ULL, 0x362f12667f1d7294ULL, 0xd33246dc9187feffULL, 0x638e688403830bffULL,
      0x8b349f95eb17ee0bULL, 0x3b88b1cd79131b0bULL, 0xde95e57797899760ULL, 0x6e29cb2f058d6260ULL,
      0x20766a51122b1cddULL, 0x90ca4409802fe9ddULL, 0x75d710b36eb565b6ULL, 0xc56b3eebfcb190b6ULL,
      0xe968524f41f998ccULL, 0x59d47c17d3fd6dccULL, 0xbcc928ad3d67e1a7ULL, 0x0c7506f5af6314a7ULL,
      0x422aa78bb8c56a1aULL, 0xf29689d32ac19f1aULL, 0x178bdd69c45b1371ULL, 0xa737f331565fe671ULL,
      0xf22715194de54bf2ULL, 0x429b3b41dfe1bef2ULL, 0xa7866ffb317b3299ULL, 0x173a41a3a37fc799ULL,
      0x5965e0ddb4d9b924ULL, 0xe9d9ce8526dd4c24ULL, 0x0cc49a3fc847c04fULL, 0xbc78b4675a43354fULL,
      0x907bd8c3e70b3d35ULL, 0x20c7f69b750fc835ULL, 0xc5daa2219b95445eULL, 0x75668c790991b15eULL,
      0x3b392d071e37cfe3ULL, 0x8b85035f8c333ae3ULL, 0x6e9857e562a9b688ULL, 0xde2479bdf0ad4388ULL,
      0x369e8eac1839a67cULL, 0x8622a0f48a3d537cULL, 0x633ff44e64a7df17ULL, 0xd383da16f6a32a17ULL,
      0x9ddc7b68e10554aaULL, 0x2d6055307301a1aaULL, 0xc87d018a9d9b2dc1ULL, 0x78c12fd20f9fd8c1ULL,
      0x54c24376b2d7d0bbULL, 0xe47e6d2e20d325bbULL, 0x01633994ce49a9d0ULL, 0xb1df17cc5c4d5cd0ULL,
      0xff80b6b24beb226dULL, 0x4f3c98ead9efd76dULL, 0xaa21cc5037755b06ULL, 0x1a9de208a571ae06ULL,
      0x9f1a08417d96070aULL, 0x2fa62619ef92f20aULL, 0xcabb72a301087e61ULL, 0x7a075cfb930c8b61ULL,
      0x3458fd8584aaf5dcULL, 0x84e4d3dd16ae00dcULL, 0x61f98767f8348cb7ULL, 0xd145a93f6a3079b7ULL,
      0xfd46c59bd77871cdULL, 0x4dfaebc3457c84cdULL, 0xa8e7bf79abe608a6ULL, 0x185b912139e2fda6ULL,
      0x5604305f2e44831bULL, 0xe6b81e07bc40761bULL, 0x03a54abd52dafa70ULL, 0xb31964e5c0de0f70ULL,
      0x5ba393f4284aea84ULL, 0xeb1fbdacba4e1f84ULL, 0x0e02e91654d493efULL, 0xbebec74ec6d066efULL,
      0xf0e16630d1761852ULL, 0x405d48684372ed52ULL, 0xa5401cd2ade86139ULL, 0x15fc328a3fec9439ULL,
      0x39ff5e2e82a49c43ULL, 0x8943707610a06943ULL, 0x6c5e24ccfe3ae528ULL, 0xdce20a946c3e1028ULL,
      0x92bdabea7b986e95ULL, 0x220185b2e99c9b95ULL, 0xc71cd108070617feULL, 0x77a0ff509502e2feULL,
      0x22b019788eb84f7dULL, 0x920c37201cbcba7dULL, 0x7711639af2263616ULL, 0xc7ad4dc26022c316ULL,
      0x89f2ecbc7784bdabULL, 0x394ec2e4e58048abULL, 0xdc53965e0b1ac4c0ULL, 0x6cefb806991e31c0ULL,
      0x40ecd4a2245639baULL, 0xf050fafab652ccbaULL, 0x154dae4058c840d1ULL, 0xa5f18018caccb5d1ULL,
      0xebae2166dd6acb6cULL, 0x5b120f3e4f6e3e6cULL, 0xbe0f5b84a1f4b207ULL, 0x0eb375dc33f04707ULL,
      0xe60982cddb64a2f3ULL, 0x56b5ac95496057f3ULL, 0xb3a8f82fa7fadb98ULL, 0x0314d67735fe2e98ULL,
      0x4d4b770922585025ULL, 0xfdf75951b05ca525ULL, 0x18ea0deb5ec6294eULL, 0xa85623b3ccc2dc4eULL,
      0x84554f17718ad434ULL, 0x34e9614fe38e2134ULL, 0xd1f435f50d14ad5fULL, 0x61481bad9f10585fULL,
      0x2f17bad388b626e2ULL, 0x9fab948b1ab2d3e2ULL, 0x7ab6c031f4285f89ULL, 0xca0aee69662caa89ULL,
      0xd0970c61c35d048fULL, 0x602b22395159f18fULL, 0x85367683bfc37de4ULL, 0x358a58db2dc788e4ULL,
      0x7bd5f9a53a61f659ULL, 0xcb69d7fda8650359ULL, 0x2e74834746ff8f32ULL, 0x9ec8ad1fd4fb7a32ULL,
      0xb2cbc1bb69b37248ULL, 0x0277efe3fbb78748ULL, 0xe76abb59152d0b23ULL, 0x57d695018729fe23ULL,
      0x1989347f908f809eULL, 0xa9351a27028b759eULL, 0x4c284e9dec11f9f5ULL, 0xfc9460c57e150cf5ULL,
      0x142e97d49681e901ULL, 0xa492b98c04851c01ULL, 0x418fed36ea1f906aULL, 0xf133c36e781b656aULL,
      0xbf6c62106fbd1bd7ULL, 0x0fd04c48fdb9eed7ULL, 0xeacd18f2132362bcULL, 0x5a7136aa812797bcULL,
      0x76725a0e3c6f9fc6ULL, 0xc6ce7456ae6b6ac6ULL, 0x23d320ec40f1e6adULL, 0x936f0eb4d2f513adULL,
      0xdd30afcac5536d10ULL, 0x6d8c819257579810ULL, 0x8891d528b9cd147bULL, 0x382dfb702bc9e17bULL,
      0x6d3d1d5830734cf8ULL, 0xdd813300a277b9f8ULL, 0x389c67ba4ced3593ULL, 0x882049e2dee9c093ULL,
      0xc67fe89cc94fbe2eULL, 0x76c3c6c45b4b4b2eULL, 0x93de927eb5d1c745ULL, 0x2362bc2627d53245ULL,
      0x0f61d0829a9d3a3fULL, 0xbfddfeda0899cf3fULL, 0x5ac0aa60e6034354ULL, 0xea7c84387407b654ULL,
      0xa423254663a1c8e9ULL, 0x149f0b1ef1a53de9ULL, 0xf1825fa41f3fb182ULL, 0x413e71fc8d3b4482ULL,
      0xa98486ed65afa176ULL, 0x1938a8b5f7ab5476ULL, 0xfc25fc0f1931d81dULL, 0x4c99d2578b352d1dULL,
      0x02c673299c9353a0ULL, 0xb27a5d710e97a6a0ULL, 0x576709cbe00d2acbULL, 0xe7db27937209dfcbULL,
      0xcbd84b37cf41d7b1ULL, 0x7b64656f5d4522b1ULL, 0x9e7931d5b3dfaedaULL, 0x2ec51f8d21db5bdaULL,
      0x609abef3367d2567ULL, 0xd02690aba479d067ULL, 0x353bc4114ae35c0cULL, 0x8587ea49d8e7a90cULL,
  };

  static constexpr uint64_t Crc64MUX2N[] = {
      0x0080000000000000UL, 0x0000800000000000UL, 0x0000000080000000UL, 0x9a6c9329ac4bc9b5UL,
      0x10f4bb0f129310d6UL, 0x70f05dcea2ebd226UL, 0x311211205672822dUL, 0x2fc297db0f46c96eUL,
      0xca4d536fabf7da84UL, 0xfb4cdc3b379ee6edUL, 0xea261148df25140aUL, 0x59ccb2c07aa6c9b4UL,
      0x20b3674a839af27aUL, 0x2d8e1986da94d583UL, 0x42cdf4c20337635dUL, 0x1d78724bf0f26839UL,
      0xb96c84e0afb34bd5UL, 0x5d2e1fcd2df0a3eaUL, 0xcd9506572332be42UL, 0x23bda2427f7d690fUL,
      0x347a953232374f07UL, 0x1c2a807ac2a8ceeaUL, 0x9b92ad0e14fe1460UL, 0x2574114889f670b2UL,
      0x4a84a6c45e3bf520UL, 0x915bbac21cd1c7ffUL, 0xb0290ec579f291f5UL, 0xcf2548505c624e6eUL,
      0xb154f27bf08a8207UL, 0xce4e92344baf7d35UL, 0x51da8d7e057c5eb3UL, 0x9fb10823f5be15dfUL,
      0x73b825b3ff1f71cfUL, 0x5db436c5406ebb74UL, 0xfa7ed8f3ec3f2bcaUL, 0xc4d58efdc61b9ef6UL,
      0xa7e39e61e855bd45UL, 0x97ad46f9dd1bf2f1UL, 0x1a0abb01f853ee6bUL, 0x3f0827c3348f8215UL,
      0x4eb68c4506134607UL, 0x4a46f6de5df34e0aUL, 0x2d855d6a1c57a8ddUL, 0x8688da58e1115812UL,
      0x5232f417fc7c7300UL, 0xa4080fb2e767d8daUL, 0xd515a7e17693e562UL, 0x1181f7c862e94226UL,
      0x9e23cd058204ca91UL, 0x9b8992c57a0aed82UL, 0xb2c0afb84609b6ffUL, 0x2f7160553a5ea018UL,
      0x3cd378b5c99f2722UL, 0x814054ad61a3b058UL, 0xbf766189fce806d8UL, 0x85a5e898ac49f86fUL,
      0x34830d11bc84f346UL, 0x9644d95b173c8c1cUL, 0x150401ac9ac759b1UL, 0xebe1f7f46fb00ebaUL,
      0x8ee4ce0c2e2bd662UL, 0x4000000000000000UL, 0x2000000000000000UL, 0x0800000000000000UL,
  };

  static uint64_t Crc64MulPoly(uint64_t a, uint64_t b)
  {
    constexpr uint64_t p = Crc64Poly;
    constexpr uint64_t p2 = (p >> 1) ^ (p * (p & 1));
    constexpr uint64_t bw = sizeof(p) * 8;
    constexpr uint64_t vt[] = {0, p2, p, p ^ p2};
    constexpr uint64_t vs[] = {bw - 2, bw - 1};
    uint64_t vb[] = {(b >> 1) ^ vt[(b & 1) << 1], b};
    uint64_t vr[] = {0, 0};
    for (uint64_t i = 0; i < bw; i += 2)
    {
      for (int j = 0; j < 2; ++j)
      {
        vr[j] ^= vb[j] * ((a >> vs[j]) & 1);
        vb[j] = (vb[j] >> 2) ^ vt[vb[j] & 3];
      }
      a <<= 2;
    }

    return vr[0] ^ vr[1];
  }

  void Crc64Hash::OnAppend(const uint8_t* data, size_t length)
  {
    m_length += length;

    uint64_t uCrc = m_context ^ ~0ULL;

    uint64_t pData = 0;

    size_t uStop = length - (length % 32);
    if (uStop >= 2 * 32)
    {
      const uint64_t* wData = reinterpret_cast<const uint64_t*>(data);

      uint64_t uCrc0 = 0;
      uint64_t uCrc1 = 0;
      uint64_t uCrc2 = 0;
      uint64_t uCrc3 = 0;
      uint64_t pLast = pData + uStop - 32;
      length -= uStop;
      uCrc0 = uCrc;

      for (; pData < pLast; pData += 32, wData += 4)
      {
        uint64_t b0 = wData[0] ^ uCrc0;
        uint64_t b1 = wData[1] ^ uCrc1;
        uint64_t b2 = wData[2] ^ uCrc2;
        uint64_t b3 = wData[3] ^ uCrc3;

        uCrc0 = Crc64MU32[7 * 256 + (b0 & 0xff)];
        b0 >>= 8;
        uCrc1 = Crc64MU32[7 * 256 + (b1 & 0xff)];
        b1 >>= 8;
        uCrc2 = Crc64MU32[7 * 256 + (b2 & 0xff)];
        b2 >>= 8;
        uCrc3 = Crc64MU32[7 * 256 + (b3 & 0xff)];
        b3 >>= 8;

        uCrc0 ^= Crc64MU32[6 * 256 + (b0 & 0xff)];
        b0 >>= 8;
        uCrc1 ^= Crc64MU32[6 * 256 + (b1 & 0xff)];
        b1 >>= 8;
        uCrc2 ^= Crc64MU32[6 * 256 + (b2 & 0xff)];
        b2 >>= 8;
        uCrc3 ^= Crc64MU32[6 * 256 + (b3 & 0xff)];
        b3 >>= 8;

        uCrc0 ^= Crc64MU32[5 * 256 + (b0 & 0xff)];
        b0 >>= 8;
        uCrc1 ^= Crc64MU32[5 * 256 + (b1 & 0xff)];
        b1 >>= 8;
        uCrc2 ^= Crc64MU32[5 * 256 + (b2 & 0xff)];
        b2 >>= 8;
        uCrc3 ^= Crc64MU32[5 * 256 + (b3 & 0xff)];
        b3 >>= 8;

        uCrc0 ^= Crc64MU32[4 * 256 + (b0 & 0xff)];
        b0 >>= 8;
        uCrc1 ^= Crc64MU32[4 * 256 + (b1 & 0xff)];
        b1 >>= 8;
        uCrc2 ^= Crc64MU32[4 * 256 + (b2 & 0xff)];
        b2 >>= 8;
        uCrc3 ^= Crc64MU32[4 * 256 + (b3 & 0xff)];
        b3 >>= 8;

        uCrc0 ^= Crc64MU32[3 * 256 + (b0 & 0xff)];
        b0 >>= 8;
        uCrc1 ^= Crc64MU32[3 * 256 + (b1 & 0xff)];
        b1 >>= 8;
        uCrc2 ^= Crc64MU32[3 * 256 + (b2 & 0xff)];
        b2 >>= 8;
        uCrc3 ^= Crc64MU32[3 * 256 + (b3 & 0xff)];
        b3 >>= 8;

        uCrc0 ^= Crc64MU32[2 * 256 + (b0 & 0xff)];
        b0 >>= 8;
        uCrc1 ^= Crc64MU32[2 * 256 + (b1 & 0xff)];
        b1 >>= 8;
        uCrc2 ^= Crc64MU32[2 * 256 + (b2 & 0xff)];
        b2 >>= 8;
        uCrc3 ^= Crc64MU32[2 * 256 + (b3 & 0xff)];
        b3 >>= 8;

        uCrc0 ^= Crc64MU32[1 * 256 + (b0 & 0xff)];
        b0 >>= 8;
        uCrc1 ^= Crc64MU32[1 * 256 + (b1 & 0xff)];
        b1 >>= 8;
        uCrc2 ^= Crc64MU32[1 * 256 + (b2 & 0xff)];
        b2 >>= 8;
        uCrc3 ^= Crc64MU32[1 * 256 + (b3 & 0xff)];
        b3 >>= 8;

        uCrc0 ^= Crc64MU32[0 * 256 + (b0 & 0xff)];
        uCrc1 ^= Crc64MU32[0 * 256 + (b1 & 0xff)];
        uCrc2 ^= Crc64MU32[0 * 256 + (b2 & 0xff)];
        uCrc3 ^= Crc64MU32[0 * 256 + (b3 & 0xff)];
      }

      uCrc = 0;
      uCrc ^= wData[0] ^ uCrc0;
      uCrc = (uCrc >> 8) ^ Crc64MU1[uCrc & 0xff];
      uCrc = (uCrc >> 8) ^ Crc64MU1[uCrc & 0xff];
      uCrc = (uCrc >> 8) ^ Crc64MU1[uCrc & 0xff];
      uCrc = (uCrc >> 8) ^ Crc64MU1[uCrc & 0xff];
      uCrc = (uCrc >> 8) ^ Crc64MU1[uCrc & 0xff];
      uCrc = (uCrc >> 8) ^ Crc64MU1[uCrc & 0xff];
      uCrc = (uCrc >> 8) ^ Crc64MU1[uCrc & 0xff];
      uCrc = (uCrc >> 8) ^ Crc64MU1[uCrc & 0xff];

      uCrc ^= wData[1] ^ uCrc1;
      uCrc = (uCrc >> 8) ^ Crc64MU1[uCrc & 0xff];
      uCrc = (uCrc >> 8) ^ Crc64MU1[uCrc & 0xff];
      uCrc = (uCrc >> 8) ^ Crc64MU1[uCrc & 0xff];
      uCrc = (uCrc >> 8) ^ Crc64MU1[uCrc & 0xff];
      uCrc = (uCrc >> 8) ^ Crc64MU1[uCrc & 0xff];
      uCrc = (uCrc >> 8) ^ Crc64MU1[uCrc & 0xff];
      uCrc = (uCrc >> 8) ^ Crc64MU1[uCrc & 0xff];
      uCrc = (uCrc >> 8) ^ Crc64MU1[uCrc & 0xff];

      uCrc ^= wData[2] ^ uCrc2;
      uCrc = (uCrc >> 8) ^ Crc64MU1[uCrc & 0xff];
      uCrc = (uCrc >> 8) ^ Crc64MU1[uCrc & 0xff];
      uCrc = (uCrc >> 8) ^ Crc64MU1[uCrc & 0xff];
      uCrc = (uCrc >> 8) ^ Crc64MU1[uCrc & 0xff];
      uCrc = (uCrc >> 8) ^ Crc64MU1[uCrc & 0xff];
      uCrc = (uCrc >> 8) ^ Crc64MU1[uCrc & 0xff];
      uCrc = (uCrc >> 8) ^ Crc64MU1[uCrc & 0xff];
      uCrc = (uCrc >> 8) ^ Crc64MU1[uCrc & 0xff];

      uCrc ^= wData[3] ^ uCrc3;
      uCrc = (uCrc >> 8) ^ Crc64MU1[uCrc & 0xff];
      uCrc = (uCrc >> 8) ^ Crc64MU1[uCrc & 0xff];
      uCrc = (uCrc >> 8) ^ Crc64MU1[uCrc & 0xff];
      uCrc = (uCrc >> 8) ^ Crc64MU1[uCrc & 0xff];
      uCrc = (uCrc >> 8) ^ Crc64MU1[uCrc & 0xff];
      uCrc = (uCrc >> 8) ^ Crc64MU1[uCrc & 0xff];
      uCrc = (uCrc >> 8) ^ Crc64MU1[uCrc & 0xff];
      uCrc = (uCrc >> 8) ^ Crc64MU1[uCrc & 0xff];

      pData += 32;
    }

    for (uint64_t uBytes = 0; uBytes < length; ++uBytes, ++pData)
    {
      uCrc = (uCrc >> 8) ^ Crc64MU1[(uCrc ^ data[pData]) & 0xff];
    }
    m_context = uCrc ^ ~0ULL;
  }

  void Crc64Hash::Concatenate(const Crc64Hash& other)
  {
    m_length += other.m_length;

    /*
    // The same effect as
    m_context ^= ~0ULL;
    std::vector<uint8_t> zerostr(other.m_length, '\x00');
    Update(zerostr.data(), zerostr.size());
    m_context ^= other.m_context;
    m_context ^= ~0ULL;
    */

    {
      uint64_t i = 0;
      uint64_t r = m_context;
      uint64_t s = other.m_length;
      for (s >>= i; s != 0; s >>= 1, ++i)
      {
        if ((s & 1) == 1)
        {
          r = Crc64MulPoly(r, Crc64MUX2N[i]);
        }
      }
      m_context = r;
    }
    m_context ^= other.m_context;
  }

  std::vector<uint8_t> Crc64Hash::OnFinal(const uint8_t* data, size_t length)
  {
    OnAppend(data, length);
    std::vector<uint8_t> binary;
    binary.resize(sizeof(m_context));
    for (size_t i = 0; i < sizeof(m_context); ++i)
    {
      binary[i] = (m_context >> (8 * i)) & 0xff;
    }
    return binary;
  }

}} // namespace Azure::Storage
