// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "azure/core/base64.hpp"

#include "azure/core/platform.hpp"

#include <string>
#include <vector>

namespace {

static char const Base64EncodeArray[65]
    = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static char const EncodingPad = '=';
static int8_t const Base64DecodeArray[256] = {
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    62,
    -1,
    -1,
    -1,
    63, // 62 is placed at index 43 (for +), 63 at index 47 (for /)
    52,
    53,
    54,
    55,
    56,
    57,
    58,
    59,
    60,
    61,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1, // 52-61 are placed at index 48-57 (for 0-9), 64 at index 61 (for =)
    -1,
    0,
    1,
    2,
    3,
    4,
    5,
    6,
    7,
    8,
    9,
    10,
    11,
    12,
    13,
    14,
    15,
    16,
    17,
    18,
    19,
    20,
    21,
    22,
    23,
    24,
    25,
    -1,
    -1,
    -1,
    -1,
    -1, // 0-25 are placed at index 65-90 (for A-Z)
    -1,
    26,
    27,
    28,
    29,
    30,
    31,
    32,
    33,
    34,
    35,
    36,
    37,
    38,
    39,
    40,
    41,
    42,
    43,
    44,
    45,
    46,
    47,
    48,
    49,
    50,
    51,
    -1,
    -1,
    -1,
    -1,
    -1, // 26-51 are placed at index 97-122 (for a-z)
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1, // Bytes over 122 ('z') are invalid and cannot be decoded. Hence, padding the map with -1,
        // which indicates invalid input
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
};

static int32_t Base64Encode(const uint8_t* threeBytes)
{
  int32_t i = (threeBytes[0] << 16) | (threeBytes[1] << 8) | threeBytes[2];

  int32_t i0 = Base64EncodeArray[i >> 18];
  int32_t i1 = Base64EncodeArray[(i >> 12) & 0x3F];
  int32_t i2 = Base64EncodeArray[(i >> 6) & 0x3F];
  int32_t i3 = Base64EncodeArray[i & 0x3F];

  return i0 | (i1 << 8) | (i2 << 16) | (i3 << 24);
}

static int32_t Base64EncodeAndPadOne(const uint8_t* twoBytes)
{
  int32_t i = twoBytes[0] << 16 | (twoBytes[1] << 8);

  int32_t i0 = Base64EncodeArray[i >> 18];
  int32_t i1 = Base64EncodeArray[(i >> 12) & 0x3F];
  int32_t i2 = Base64EncodeArray[(i >> 6) & 0x3F];

  return i0 | (i1 << 8) | (i2 << 16) | (EncodingPad << 24);
}

static int32_t Base64EncodeAndPadTwo(const uint8_t* oneByte)
{
  int32_t i = oneByte[0] << 8;

  int32_t i0 = Base64EncodeArray[i >> 10];
  int32_t i1 = Base64EncodeArray[(i >> 4) & 0x3F];

  return i0 | (i1 << 8) | (EncodingPad << 16) | (EncodingPad << 24);
}

static void Base64WriteIntAsFourBytes(char* destination, int32_t value)
{
  destination[3] = static_cast<uint8_t>((value >> 24) & 0xFF);
  destination[2] = static_cast<uint8_t>((value >> 16) & 0xFF);
  destination[1] = static_cast<uint8_t>((value >> 8) & 0xFF);
  destination[0] = static_cast<uint8_t>(value & 0xFF);
}

std::string Base64Encode(uint8_t const* const data, size_t length)
{
  size_t sourceIndex = 0;
  auto inputSize = length;
  auto maxEncodedSize = ((inputSize + 2) / 3) * 4;
  // Use a string with size to the max possible result
  std::string encodedResult(maxEncodedSize, '0');
  // Removing const from the string to update the placeholder string
  auto destination = const_cast<char*>(encodedResult.data());

  while (sourceIndex + 3 <= inputSize)
  {
    int32_t result = Base64Encode(&data[sourceIndex]);
    Base64WriteIntAsFourBytes(destination, result);
    destination += 4;
    sourceIndex += 3;
  }

  if (sourceIndex + 1 == inputSize)
  {
    int32_t result = Base64EncodeAndPadTwo(&data[sourceIndex]);
    Base64WriteIntAsFourBytes(destination, result);
    destination += 4;
    sourceIndex += 1;
  }
  else if (sourceIndex + 2 == inputSize)
  {
    int32_t result = Base64EncodeAndPadOne(&data[sourceIndex]);
    Base64WriteIntAsFourBytes(destination, result);
    destination += 4;
    sourceIndex += 2;
  }

  return encodedResult;
}

static int32_t Base64Decode(const char* encodedBytes)
{
  int32_t i0 = encodedBytes[0];
  int32_t i1 = encodedBytes[1];
  int32_t i2 = encodedBytes[2];
  int32_t i3 = encodedBytes[3];

  i0 = Base64DecodeArray[i0];
  i1 = Base64DecodeArray[i1];
  i2 = Base64DecodeArray[i2];
  i3 = Base64DecodeArray[i3];

  i0 <<= 18;
  i1 <<= 12;
  i2 <<= 6;

  i0 |= i3;
  i1 |= i2;

  i0 |= i1;
  return i0;
}

static void Base64WriteThreeLowOrderBytes(std::vector<uint8_t>::iterator destination, int64_t value)
{
  destination[0] = static_cast<uint8_t>(value >> 16);
  destination[1] = static_cast<uint8_t>(value >> 8);
  destination[2] = static_cast<uint8_t>(value);
}

std::vector<uint8_t> Base64Decode(const std::string& text)
{
  auto inputSize = text.size();
  if (inputSize % 4 != 0)
  {
    throw std::runtime_error("Unexpected end of Base64 encoded string.");
  }

  // An empty input should result in an empty output.
  if (inputSize == 0)
  {
    return std::vector<uint8_t>();
  }

  size_t sourceIndex = 0;
  auto inputPtr = text.data();
  auto decodedSize = (inputSize / 4) * 3;

  if (inputPtr[inputSize - 2] == EncodingPad)
  {
    decodedSize -= 2;
  }
  else if (inputPtr[inputSize - 1] == EncodingPad)
  {
    decodedSize -= 1;
  }

  std::vector<uint8_t> destination(decodedSize);
  auto destinationPtr = destination.begin();

  while (sourceIndex + 4 < inputSize)
  {
    int64_t result = Base64Decode(inputPtr + sourceIndex);
    if (result < 0)
    {
      throw std::runtime_error("Unexpected character in Base64 encoded string");
    }
    Base64WriteThreeLowOrderBytes(destinationPtr, result);
    destinationPtr += 3;
    sourceIndex += 4;
  }

  // We are guaranteed to have an input with at least 4 bytes at this point, with a size that is a
  // multiple of 4.
  int64_t i0 = inputPtr[inputSize - 4];
  int64_t i1 = inputPtr[inputSize - 3];
  int64_t i2 = inputPtr[inputSize - 2];
  int64_t i3 = inputPtr[inputSize - 1];

  i0 = Base64DecodeArray[i0];
  i1 = Base64DecodeArray[i1];

  i0 <<= 18;
  i1 <<= 12;

  i0 |= i1;

  if (i3 != EncodingPad)
  {
    i2 = Base64DecodeArray[i2];
    i3 = Base64DecodeArray[i3];

    i2 <<= 6;

    i0 |= i3;
    i0 |= i2;

    if (i0 < 0)
    {
      throw std::runtime_error("Unexpected character in Base64 encoded string");
    }

    Base64WriteThreeLowOrderBytes(destinationPtr, i0);
    destinationPtr += 3;
  }
  else if (i2 != EncodingPad)
  {
    i2 = Base64DecodeArray[i2];

    i2 <<= 6;

    i0 |= i2;
    if (i0 < 0)
    {
      throw std::runtime_error("Unexpected character in Base64 encoded string");
    }

    destinationPtr[1] = static_cast<uint8_t>(i0 >> 8);
    destinationPtr[0] = static_cast<uint8_t>(i0 >> 16);
    destinationPtr += 2;
  }
  else
  {
    if (i0 < 0)
    {
      throw std::runtime_error("Unexpected character in Base64 encoded string");
    }

    destinationPtr[0] = static_cast<uint8_t>(i0 >> 16);
    destinationPtr += 1;
  }

  return destination;
}

} // namespace

namespace Azure { namespace Core {

  std::string Convert::Base64Encode(const std::vector<uint8_t>& data)
  {
    return ::Base64Encode(data.data(), data.size());
  }

  std::vector<uint8_t> Convert::Base64Decode(const std::string& text)
  {
    return ::Base64Decode(text);
  }
  namespace _internal {

    std::string Convert::Base64Encode(const std::string& data)
    {
      return ::Base64Encode(reinterpret_cast<const uint8_t*>(data.data()), data.size());
    }
  } // namespace _internal

}} // namespace Azure::Core
