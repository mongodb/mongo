// Copyright 2020 The Abseil Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "absl/hash/internal/wyhash.h"

#include "absl/strings/escaping.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace {

static const uint64_t kCurrentSeed = 0;
static const uint64_t kSalt[5] = {0xa0761d6478bd642f, 0xe7037ed1a0b428dbl,
                                  0x8ebc6af09c88c6e3, 0x589965cc75374cc3l,
                                  0x1d8e4e27c47d124f};

// Note: We don't account for endianness, so the values here are only correct if
// you're also running on a little endian platform.

TEST(WyhashTest, EmptyString) {
  const std::string s = "";
  EXPECT_EQ(
      absl::hash_internal::Wyhash(s.c_str(), s.length(), kCurrentSeed, kSalt),
      4808886099364463827);
}

TEST(WyhashTest, Spaces) {
  const std::string s = "   ";
  EXPECT_EQ(
      absl::hash_internal::Wyhash(s.c_str(), s.length(), kCurrentSeed, kSalt),
      1686201463024549249);
}

TEST(WyhashTest, RepeatingString) {
  const std::string s = "aaaa";
  EXPECT_EQ(
      absl::hash_internal::Wyhash(s.c_str(), s.length(), kCurrentSeed, kSalt),
      6646112255271966632);
}

TEST(WyhashTest, HexString) {
  const std::string small = "\x01\x02\x03";
  const std::string med = "\x01\x02\x03\x04";

  EXPECT_EQ(absl::hash_internal::Wyhash(small.c_str(), small.length(),
                                        kCurrentSeed, kSalt),
            11989428023081740911ULL);
  EXPECT_EQ(absl::hash_internal::Wyhash(med.c_str(), med.length(), kCurrentSeed,
                                        kSalt),
            9765997711188871556ULL);
}

TEST(WyhashTest, Words) {
  const std::string s = "third_party|wyhash|64";
  EXPECT_EQ(
      absl::hash_internal::Wyhash(s.c_str(), s.length(), kCurrentSeed, kSalt),
      3702018632387611330);
}

TEST(WyhashTest, LongString) {
  const std::string s =
      "AbCdEfGhIjKlMnOpQrStUvWxYz0123456789AbCdEfGhIjKlMnOpQrStUvWxYz"
      "0123456789AbCdEfGhIjKlMnOpQrStUvWxYz0123456789AbCdEfGhIjKlMnOp"
      "QrStUvWxYz0123456789AbCdEfGhIjKlMnOpQrStUvWxYz0123456789AbCdEf"
      "GhIjKlMnOpQrStUvWxYz0123456789AbCdEfGhIjKlMnOpQrStUvWxYz012345"
      "6789AbCdEfGhIjKlMnOpQrStUvWxYz0123456789";

  EXPECT_EQ(
      absl::hash_internal::Wyhash(s.c_str(), s.length(), kCurrentSeed, kSalt),
      9245411362605796064ULL);
}

TEST(WyhashTest, BigReference) {
  struct ExpectedResult {
    absl::string_view base64_data;
    uint64_t seed;
    uint64_t hash;
  } expected_results[] = {
      {"", uint64_t{0xec42b7ab404b8acb}, uint64_t{0xe5a40d39ab796423}},
      {"Zw==", uint64_t{0xeeee074043a3ee0f}, uint64_t{0xa6564b468248c683}},
      {"xmk=", uint64_t{0x857902089c393de}, uint64_t{0xef192f401b116e1c}},
      {"c1H/", uint64_t{0x993df040024ca3af}, uint64_t{0xbe8dc0c54617639d}},
      {"SuwpzQ==", uint64_t{0xc4e4c2acea740e96}, uint64_t{0x93d7f665b5521c8e}},
      {"uqvy++M=", uint64_t{0x6a214b3db872d0cf}, uint64_t{0x646d70bb42445f28}},
      {"RnzCVPgb", uint64_t{0x44343db6a89dba4d}, uint64_t{0x96a7b1e3cc9bd426}},
      {"6OeNdlouYw==", uint64_t{0x77b5d6d1ae1dd483},
       uint64_t{0x76020289ab0790c4}},
      {"M5/JmmYyDbc=", uint64_t{0x89ab8ecb44d221f1},
       uint64_t{0x39f842e4133b9b44}},
      {"MVijWiVdBRdY", uint64_t{0x60244b17577ca81b},
       uint64_t{0x2b8d7047be4bcaab}},
      {"6V7Uq7LNxpu0VA==", uint64_t{0x59a08dcee0717067},
       uint64_t{0x99628abef6716a97}},
      {"EQ6CdEEhPdyHcOk=", uint64_t{0xf5f20db3ade57396},
       uint64_t{0x4432e02ba42b2740}},
      {"PqFB4fxnPgF+l+rc", uint64_t{0xbf8dee0751ad3efb},
       uint64_t{0x74d810efcad7918a}},
      {"a5aPOFwq7LA7+zKvPA==", uint64_t{0x6b7a06b268d63e30},
       uint64_t{0x88c84e986002507f}},
      {"VOwY21wCGv5D+/qqOvs=", uint64_t{0xb8c37f0ae0f54c82},
       uint64_t{0x4f99acf193cf39b9}},
      {"KdHmBTx8lHXYvmGJ+Vy7", uint64_t{0x9fcbed0c38e50eef},
       uint64_t{0xd90e7a3655891e37}},
      {"qJkPlbHr8bMF7/cA6aE65Q==", uint64_t{0x2af4bade1d8e3a1d},
       uint64_t{0x3bb378b1d4df8fcf}},
      {"ygvL0EhHZL0fIx6oHHtkxRQ=", uint64_t{0x714e3aa912da2f2c},
       uint64_t{0xf78e94045c052d47}},
      {"c1rFXkt5YztwZCQRngncqtSs", uint64_t{0xf5ee75e3cbb82c1c},
       uint64_t{0x26da0b2130da6b40}},
      {"8hsQrzszzeNQSEcVXLtvIhm6mw==", uint64_t{0x620e7007321b93b9},
       uint64_t{0x30b4d426af8c6986}},
      {"ffUL4RocfyP4KfikGxO1yk7omDI=", uint64_t{0xc08528cac2e551fc},
       uint64_t{0x5413b4aaf3baaeae}},
      {"OOB5TT00vF9Od/rLbAWshiErqhpV", uint64_t{0x6a1debf9cc3ad39},
       uint64_t{0x756ab265370a1597}},
      {"or5wtXM7BFzTNpSzr+Lw5J5PMhVJ/Q==", uint64_t{0x7e0a3c88111fc226},
       uint64_t{0xdaf5f4b7d09814fb}},
      {"gk6pCHDUsoopVEiaCrzVDhioRKxb844=", uint64_t{0x1301fef15df39edb},
       uint64_t{0x8f874ae37742b75e}},
      {"TNctmwlC5QbEM6/No4R/La3UdkfeMhzs", uint64_t{0x64e181f3d5817ab},
       uint64_t{0x8fecd03956121ce8}},
      {"SsQw9iAjhWz7sgcE9OwLuSC6hsM+BfHs2Q==", uint64_t{0xafafc44961078ecb},
       uint64_t{0x229c292ea7a08285}},
      {"ZzO3mVCj4xTT2TT3XqDyEKj2BZQBvrS8RHg=", uint64_t{0x4f7bb45549250094},
       uint64_t{0xbb4bf0692d14bae}},
      {"+klp5iPQGtppan5MflEls0iEUzqU+zGZkDJX", uint64_t{0xa30061abaa2818c},
       uint64_t{0x207b24ca3bdac1db}},
      {"RO6bvOnlJc8I9eniXlNgqtKy0IX6VNg16NRmgg==", uint64_t{0xd902ee3e44a5705f},
       uint64_t{0x64f6cd6745d3825b}},
      {"ZJjZqId1ZXBaij9igClE3nyliU5XWdNRrayGlYA=", uint64_t{0x316d36da516f583},
       uint64_t{0xa2b2e1656b58df1e}},
      {"7BfkhfGMDGbxfMB8uyL85GbaYQtjr2K8g7RpLzr/", uint64_t{0x402d83f9f834f616},
       uint64_t{0xd01d30d9ee7a148}},
      {"rycWk6wHH7htETQtje9PidS2YzXBx+Qkg2fY7ZYS7A==",
       uint64_t{0x9c604164c016b72c}, uint64_t{0x1cb4cd00ab804e3b}},
      {"RTkC2OUK+J13CdGllsH0H5WqgspsSa6QzRZouqx6pvI=",
       uint64_t{0x3f4507e01f9e73ba}, uint64_t{0x4697f2637fd90999}},
      {"tKjKmbLCNyrLCM9hycOAXm4DKNpM12oZ7dLTmUx5iwAi",
       uint64_t{0xc3fe0d5be8d2c7c7}, uint64_t{0x8383a756b5688c07}},
      {"VprUGNH+5NnNRaORxgH/ySrZFQFDL+4VAodhfBNinmn8cg==",
       uint64_t{0x531858a40bfa7ea1}, uint64_t{0x695c29cb3696a975}},
      {"gc1xZaY+q0nPcUvOOnWnT3bqfmT/geth/f7Dm2e/DemMfk4=",
       uint64_t{0x86689478a7a7e8fa}, uint64_t{0xda2e5a5a5e971521}},
      {"Mr35fIxqx1ukPAL0su1yFuzzAU3wABCLZ8+ZUFsXn47UmAph",
       uint64_t{0x4ec948b8e7f27288}, uint64_t{0x7935d4befa056b2b}},
      {"A9G8pw2+m7+rDtWYAdbl8tb2fT7FFo4hLi2vAsa5Y8mKH3CX3g==",
       uint64_t{0xce46c7213c10032}, uint64_t{0x38dd541ca95420fe}},
      {"DFaJGishGwEHDdj9ixbCoaTjz9KS0phLNWHVVdFsM93CvPft3hM=",
       uint64_t{0xf63e96ee6f32a8b6}, uint64_t{0xcc06c7a4963f967f}},
      {"7+Ugx+Kr3aRNgYgcUxru62YkTDt5Hqis+2po81hGBkcrJg4N0uuy",
       uint64_t{0x1cfe85e65fc5225}, uint64_t{0xbf0f6f66e232fb20}},
      {"H2w6O8BUKqu6Tvj2xxaecxEI2wRgIgqnTTG1WwOgDSINR13Nm4d4Vg==",
       uint64_t{0x45c474f1cee1d2e8}, uint64_t{0xf7efb32d373fe71a}},
      {"1XBMnIbqD5jy65xTDaf6WtiwtdtQwv1dCVoqpeKj+7cTR1SaMWMyI04=",
       uint64_t{0x6e024e14015f329c}, uint64_t{0xe2e64634b1c12660}},
      {"znZbdXG2TSFrKHEuJc83gPncYpzXGbAebUpP0XxzH0rpe8BaMQ17nDbt",
       uint64_t{0x760c40502103ae1c}, uint64_t{0x285b8fd1638e306d}},
      {"ylu8Atu13j1StlcC1MRMJJXIl7USgDDS22HgVv0WQ8hx/8pNtaiKB17hCQ==",
       uint64_t{0x17fd05c3c560c320}, uint64_t{0x658e8a4e3b714d6c}},
      {"M6ZVVzsd7vAvbiACSYHioH/440dp4xG2mLlBnxgiqEvI/aIEGpD0Sf4VS0g=",
       uint64_t{0x8b34200a6f8e90d9}, uint64_t{0xf391fb968e0eb398}},
      {"li3oFSXLXI+ubUVGJ4blP6mNinGKLHWkvGruun85AhVn6iuMtocbZPVhqxzn",
       uint64_t{0x6be89e50818bdf69}, uint64_t{0x744a9ea0cc144bf2}},
      {"kFuQHuUCqBF3Tc3hO4dgdIp223ShaCoog48d5Do5zMqUXOh5XpGK1t5XtxnfGA==",
       uint64_t{0xfb389773315b47d8}, uint64_t{0x12636f2be11012f1}},
      {"jWmOad0v0QhXVJd1OdGuBZtDYYS8wBVHlvOeTQx9ZZnm8wLEItPMeihj72E0nWY=",
       uint64_t{0x4f2512a23f61efee}, uint64_t{0x29c57de825948f80}},
      {"z+DHU52HaOQdW4JrZwDQAebEA6rm13Zg/9lPYA3txt3NjTBqFZlOMvTRnVzRbl23",
       uint64_t{0x59ccd92fc16c6fda}, uint64_t{0x58c6f99ab0d1c021}},
      {"MmBiGDfYeTayyJa/tVycg+rN7f9mPDFaDc+23j0TlW9094er0ADigsl4QX7V3gG/qw==",
       uint64_t{0x25c5a7f5bd330919}, uint64_t{0x13e7b5a7b82fe3bb}},
      {"774RK+9rOL4iFvs1q2qpo/JVc/I39buvNjqEFDtDvyoB0FXxPI2vXqOrk08VPfIHkmU=",
       uint64_t{0x51df4174d34c97d7}, uint64_t{0x10fbc87901e02b63}},
      {"+slatXiQ7/2lK0BkVUI1qzNxOOLP3I1iK6OfHaoxgqT63FpzbElwEXSwdsryq3UlHK0I",
       uint64_t{0x80ce6d76f89cb57}, uint64_t{0xa24c9184901b748b}},
      {"64mVTbQ47dHjHlOHGS/hjJwr/"
       "K2frCNpn87exOqMzNUVYiPKmhCbfS7vBUce5tO6Ec9osQ==",
       uint64_t{0x20961c911965f684}, uint64_t{0xcac4fd4c5080e581}},
      {"fIsaG1r530SFrBqaDj1kqE0AJnvvK8MNEZbII2Yw1OK77v0V59xabIh0B5axaz/"
       "+a2V5WpA=",
       uint64_t{0x4e5b926ec83868e7}, uint64_t{0xc38bdb7483ba68e1}},
      {"PGih0zDEOWCYGxuHGDFu9Ivbff/"
       "iE7BNUq65tycTR2R76TerrXALRosnzaNYO5fjFhTi+CiS",
       uint64_t{0x3927b30b922eecef}, uint64_t{0xdb2a8069b2ceaffa}},
      {"RnpA/"
       "zJnEnnLjmICORByRVb9bCOgxF44p3VMiW10G7PvW7IhwsWajlP9kIwNA9FjAD2GoQHk2Q="
       "=",
       uint64_t{0xbd0291284a49b61c}, uint64_t{0xdf9fe91d0d1c7887}},
      {"qFklMceaTHqJpy2qavJE+EVBiNFOi6OxjOA3LeIcBop1K7w8xQi3TrDk+"
       "BrWPRIbfprszSaPfrI=",
       uint64_t{0x73a77c575bcc956}, uint64_t{0xe83f49e96e2e6a08}},
      {"cLbfUtLl3EcQmITWoTskUR8da/VafRDYF/ylPYwk7/"
       "zazk6ssyrzxMN3mmSyvrXR2yDGNZ3WDrTT",
       uint64_t{0x766a0e2ade6d09a6}, uint64_t{0xc69e61b62ca2b62}},
      {"s/"
       "Jf1+"
       "FbsbCpXWPTUSeWyMH6e4CvTFvPE5Fs6Z8hvFITGyr0dtukHzkI84oviVLxhM1xMxrMAy1db"
       "w==",
       uint64_t{0x2599f4f905115869}, uint64_t{0xb4a4f3f85f8298fe}},
      {"FvyQ00+j7nmYZVQ8hI1Edxd0AWplhTfWuFGiu34AK5X8u2hLX1bE97sZM0CmeLe+"
       "7LgoUT1fJ/axybE=",
       uint64_t{0xd8256e5444d21e53}, uint64_t{0x167a1b39e1e95f41}},
      {"L8ncxMaYLBH3g9buPu8hfpWZNlOF7nvWLNv9IozH07uQsIBWSKxoPy8+"
       "LW4tTuzC6CIWbRGRRD1sQV/4",
       uint64_t{0xf664a91333fb8dfd}, uint64_t{0xf8a2a5649855ee41}},
      {"CDK0meI07yrgV2kQlZZ+"
       "wuVqhc2NmzqeLH7bmcA6kchsRWFPeVF5Wqjjaj556ABeUoUr3yBmfU3kWOakkg==",
       uint64_t{0x9625b859be372cd1}, uint64_t{0x27992565b595c498}},
      {"d23/vc5ONh/"
       "HkMiq+gYk4gaCNYyuFKwUkvn46t+dfVcKfBTYykr4kdvAPNXGYLjM4u1YkAEFpJP+"
       "nX7eOvs=",
       uint64_t{0x7b99940782e29898}, uint64_t{0x3e08cca5b71f9346}},
      {"NUR3SRxBkxTSbtQORJpu/GdR6b/h6sSGfsMj/KFd99ahbh+9r7LSgSGmkGVB/"
       "mGoT0pnMTQst7Lv2q6QN6Vm",
       uint64_t{0x4fe12fa5383b51a8}, uint64_t{0xad406b10c770a6d2}},
      {"2BOFlcI3Z0RYDtS9T9Ie9yJoXlOdigpPeeT+CRujb/"
       "O39Ih5LPC9hP6RQk1kYESGyaLZZi3jtabHs7DiVx/VDg==",
       uint64_t{0xe2ccb09ac0f5b4b6}, uint64_t{0xd1713ce6e552bcf2}},
      {"FF2HQE1FxEvWBpg6Z9zAMH+Zlqx8S1JD/"
       "wIlViL6ZDZY63alMDrxB0GJQahmAtjlm26RGLnjW7jmgQ4Ie3I+014=",
       uint64_t{0x7d0a37adbd7b753b}, uint64_t{0x753b287194c73ad3}},
      {"tHmO7mqVL/PX11nZrz50Hc+M17Poj5lpnqHkEN+4bpMx/"
       "YGbkrGOaYjoQjgmt1X2QyypK7xClFrjeWrCMdlVYtbW",
       uint64_t{0xd3ae96ef9f7185f2}, uint64_t{0x5ae41a95f600af1c}},
      {"/WiHi9IQcxRImsudkA/KOTqGe8/"
       "gXkhKIHkjddv5S9hi02M049dIK3EUyAEjkjpdGLUs+BN0QzPtZqjIYPOgwsYE9g==",
       uint64_t{0x4fb88ea63f79a0d8}, uint64_t{0x4a61163b86a8bb4c}},
      {"qds+1ExSnU11L4fTSDz/QE90g4Jh6ioqSh3KDOTOAo2pQGL1k/"
       "9CCC7J23YF27dUTzrWsCQA2m4epXoCc3yPHb3xElA=",
       uint64_t{0xed564e259bb5ebe9}, uint64_t{0x42eeaa79e760c7e4}},
      {"8FVYHx40lSQPTHheh08Oq0/"
       "pGm2OlG8BEf8ezvAxHuGGdgCkqpXIueJBF2mQJhTfDy5NncO8ntS7vaKs7sCNdDaNGOEi",
       uint64_t{0x3e3256b60c428000}, uint64_t{0x698df622ef465b0a}},
      {"4ZoEIrJtstiCkeew3oRzmyJHVt/pAs2pj0HgHFrBPztbQ10NsQ/"
       "lM6DM439QVxpznnBSiHMgMQJhER+70l72LqFTO1JiIQ==",
       uint64_t{0xfb05bad59ec8705}, uint64_t{0x157583111e1a6026}},
      {"hQPtaYI+wJyxXgwD5n8jGIKFKaFA/"
       "P83KqCKZfPthnjwdOFysqEOYwAaZuaaiv4cDyi9TyS8hk5cEbNP/jrI7q6pYGBLbsM=",
       uint64_t{0xafdc251dbf97b5f8}, uint64_t{0xaa1388f078e793e0}},
      {"S4gpMSKzMD7CWPsSfLeYyhSpfWOntyuVZdX1xSBjiGvsspwOZcxNKCRIOqAA0moUfOh3I5+"
       "juQV4rsqYElMD/gWfDGpsWZKQ",
       uint64_t{0x10ec9c92ddb5dcbc}, uint64_t{0xf10d68d0f3309360}},
      {"oswxop+"
       "bthuDLT4j0PcoSKby4LhF47ZKg8K17xxHf74UsGCzTBbOz0MM8hQEGlyqDT1iUiAYnaPaUp"
       "L2mRK0rcIUYA4qLt5uOw==",
       uint64_t{0x9a767d5822c7dac4}, uint64_t{0x2af056184457a3de}},
      {"0II/"
       "697p+"
       "BtLSjxj5989OXI004TogEb94VUnDzOVSgMXie72cuYRvTFNIBgtXlKfkiUjeqVpd4a+"
       "n5bxNOD1TGrjQtzKU5r7obo=",
       uint64_t{0xee46254080d6e2db}, uint64_t{0x6d0058e1590b2489}},
      {"E84YZW2qipAlMPmctrg7TKlwLZ68l4L+c0xRDUfyyFrA4MAti0q9sHq3TDFviH0Y+"
       "Kq3tEE5srWFA8LM9oomtmvm5PYxoaarWPLc",
       uint64_t{0xbbb669588d8bf398}, uint64_t{0x638f287f68817f12}},
      {"x3pa4HIElyZG0Nj7Vdy9IdJIR4izLmypXw5PCmZB5y68QQ4uRaVVi3UthsoJROvbjDJkP2D"
       "Q6L/eN8pFeLFzNPKBYzcmuMOb5Ull7w==",
       uint64_t{0xdc2afaa529beef44}, uint64_t{0xc46b71fecefd5467}},
      {"jVDKGYIuWOP/"
       "QKLdd2wi8B2VJA8Wh0c8PwrXJVM8FOGM3voPDVPyDJOU6QsBDPseoR8uuKd19OZ/"
       "zAvSCB+zlf6upAsBlheUKgCfKww=",
       uint64_t{0xf1f67391d45013a8}, uint64_t{0x2c8e94679d964e0a}},
      {"mkquunhmYe1aR2wmUz4vcvLEcKBoe6H+kjUok9VUn2+eTSkWs4oDDtJvNCWtY5efJwg/"
       "j4PgjRYWtqnrCkhaqJaEvkkOwVfgMIwF3e+d",
       uint64_t{0x16fce2b8c65a3429}, uint64_t{0x8612b797ce22503a}},
      {"fRelvKYonTQ+s+rnnvQw+JzGfFoPixtna0vzcSjiDqX5s2Kg2//"
       "UGrK+AVCyMUhO98WoB1DDbrsOYSw2QzrcPe0+3ck9sePvb+Q/IRaHbw==",
       uint64_t{0xf4b096699f49fe67}, uint64_t{0x59f929babfba7170}},
      {"DUwXFJzagljo44QeJ7/"
       "6ZKw4QXV18lhkYT2jglMr8WB3CHUU4vdsytvw6AKv42ZcG6fRkZkq9fpnmXy6xG0aO3WPT1"
       "eHuyFirAlkW+zKtwg=",
       uint64_t{0xca584c4bc8198682}, uint64_t{0x9527556923fb49a0}},
      {"cYmZCrOOBBongNTr7e4nYn52uQUy2mfe48s50JXx2AZ6cRAt/"
       "xRHJ5QbEoEJOeOHsJyM4nbzwFm++SlT6gFZZHJpkXJ92JkR86uS/eV1hJUR",
       uint64_t{0xed269fc3818b6aad}, uint64_t{0x1039ab644f5e150b}},
      {"EXeHBDfhwzAKFhsMcH9+2RHwV+mJaN01+9oacF6vgm8mCXRd6jeN9U2oAb0of5c5cO4i+"
       "Vb/LlHZSMI490SnHU0bejhSCC2gsC5d2K30ER3iNA==",
       uint64_t{0x33f253cbb8fe66a8}, uint64_t{0x7816c83f3aa05e6d}},
      {"FzkzRYoNjkxFhZDso94IHRZaJUP61nFYrh5MwDwv9FNoJ5jyNCY/"
       "eazPZk+tbmzDyJIGw2h3GxaWZ9bSlsol/vK98SbkMKCQ/wbfrXRLcDzdd/8=",
       uint64_t{0xd0b76b2c1523d99c}, uint64_t{0xf51d2f564518c619}},
      {"Re4aXISCMlYY/XsX7zkIFR04ta03u4zkL9dVbLXMa/q6hlY/CImVIIYRN3VKP4pnd0AUr/"
       "ugkyt36JcstAInb4h9rpAGQ7GMVOgBniiMBZ/MGU7H",
       uint64_t{0xfd28f0811a2a237f}, uint64_t{0x67d494cff03ac004}},
      {"ueLyMcqJXX+MhO4UApylCN9WlTQ+"
       "ltJmItgG7vFUtqs2qNwBMjmAvr5u0sAKd8jpzV0dDPTwchbIeAW5zbtkA2NABJV6hFM48ib"
       "4/J3A5mseA3cS8w==",
       uint64_t{0x6261fb136482e84}, uint64_t{0x2802d636ced1cfbb}},
      {"6Si7Yi11L+jZMkwaN+GUuzXMrlvEqviEkGOilNq0h8TdQyYKuFXzkYc/"
       "q74gP3pVCyiwz9KpVGMM9vfnq36riMHRknkmhQutxLZs5fbmOgEO69HglCU=",
       uint64_t{0x458efc750bca7c3a}, uint64_t{0xf64e20bad771cb12}},
      {"Q6AbOofGuTJOegPh9Clm/"
       "9crtUMQqylKrTc1fhfJo1tqvpXxhU4k08kntL1RG7woRnFrVh2UoMrL1kjin+s9CanT+"
       "y4hHwLqRranl9FjvxfVKm3yvg68",
       uint64_t{0xa7e69ff84e5e7c27}, uint64_t{0xb9a6cf84a83e15e}},
      {"ieQEbIPvqY2YfIjHnqfJiO1/MIVRk0RoaG/WWi3kFrfIGiNLCczYoklgaecHMm/"
       "1sZ96AjO+a5stQfZbJQwS7Sc1ODABEdJKcTsxeW2hbh9A6CFzpowP1A==",
       uint64_t{0x3c59bfd0c29efe9e}, uint64_t{0x8da6630319609301}},
      {"zQUv8hFB3zh2GGl3KTvCmnfzE+"
       "SUgQPVaSVIELFX5H9cE3FuVFGmymkPQZJLAyzC90Cmi8GqYCvPqTuAAB//"
       "XTJxy4bCcVArgZG9zJXpjowpNBfr3ngWrSE=",
       uint64_t{0x10befacc6afd298d}, uint64_t{0x40946a86e2a996f3}},
      {"US4hcC1+op5JKGC7eIs8CUgInjKWKlvKQkapulxW262E/"
       "B2ye79QxOexf188u2mFwwe3WTISJHRZzS61IwljqAWAWoBAqkUnW8SHmIDwHUP31J0p5sGd"
       "P47L",
       uint64_t{0x41d5320b0a38efa7}, uint64_t{0xcab7f5997953fa76}},
      {"9bHUWFna2LNaGF6fQLlkx1Hkt24nrkLE2CmFdWgTQV3FFbUe747SSqYw6ebpTa07MWSpWRP"
       "sHesVo2B9tqHbe7eQmqYebPDFnNqrhSdZwFm9arLQVs+7a3Ic6A==",
       uint64_t{0x58db1c7450fe17f3}, uint64_t{0x39129ca0e04fc465}},
      {"Kb3DpHRUPhtyqgs3RuXjzA08jGb59hjKTOeFt1qhoINfYyfTt2buKhD6YVffRCPsgK9SeqZ"
       "qRPJSyaqsa0ovyq1WnWW8jI/NhvAkZTVHUrX2pC+cD3OPYT05Dag=",
       uint64_t{0x6098c055a335b7a6}, uint64_t{0x5238221fd685e1b8}},
      {"gzxyMJIPlU+bJBwhFUCHSofZ/"
       "319LxqMoqnt3+L6h2U2+ZXJCSsYpE80xmR0Ta77Jq54o92SMH87HV8dGOaCTuAYF+"
       "lDL42SY1P316Cl0sZTS2ow3ZqwGbcPNs/1",
       uint64_t{0x1bbacec67845a801}, uint64_t{0x175130c407dbcaab}},
      {"uR7V0TW+FGVMpsifnaBAQ3IGlr1wx5sKd7TChuqRe6OvUXTlD4hKWy8S+"
       "8yyOw8lQabism19vOQxfmocEOW/"
       "vzY0pEa87qHrAZy4s9fH2Bltu8vaOIe+agYohhYORQ==",
       uint64_t{0xc419cfc7442190}, uint64_t{0x2f20e7536c0b0df}},
      {"1UR5eoo2aCwhacjZHaCh9bkOsITp6QunUxHQ2SfeHv0imHetzt/"
       "Z70mhyWZBalv6eAx+YfWKCUib2SHDtz/"
       "A2dc3hqUWX5VfAV7FQsghPUAtu6IiRatq4YSLpDvKZBQ=",
       uint64_t{0xc95e510d94ba270c}, uint64_t{0x2742cb488a04ad56}},
      {"opubR7H63BH7OtY+Avd7QyQ25UZ8kLBdFDsBTwZlY6gA/"
       "u+x+"
       "czC9AaZMgmQrUy15DH7YMGsvdXnviTtI4eVI4aF1H9Rl3NXMKZgwFOsdTfdcZeeHVRzBBKX"
       "8jUfh1il",
       uint64_t{0xff1ae05c98089c3f}, uint64_t{0xd6afb593879ff93b}},
      {"DC0kXcSXtfQ9FbSRwirIn5tgPri0sbzHSa78aDZVDUKCMaBGyFU6BmrulywYX8yzvwprdLs"
       "oOwTWN2wMjHlPDqrvVHNEjnmufRDblW+nSS+xtKNs3N5xsxXdv6JXDrAB/Q==",
       uint64_t{0x90c02b8dceced493}, uint64_t{0xf50ad64caac0ca7f}},
      {"BXRBk+3wEP3Lpm1y75wjoz+PgB0AMzLe8tQ1AYU2/"
       "oqrQB2YMC6W+9QDbcOfkGbeH+b7IBkt/"
       "gwCMw2HaQsRFEsurXtcQ3YwRuPz5XNaw5NAvrNa67Fm7eRzdE1+hWLKtA8=",
       uint64_t{0x9f8a76697ab1aa36}, uint64_t{0x2ade95c4261364ae}},
      {"RRBSvEGYnzR9E45Aps/+WSnpCo/X7gJLO4DRnUqFrJCV/kzWlusLE/"
       "6ZU6RoUf2ROwcgEvUiXTGjLs7ts3t9SXnJHxC1KiOzxHdYLMhVvgNd3hVSAXODpKFSkVXND"
       "55G2L1W",
       uint64_t{0x6ba1bf3d811a531d}, uint64_t{0x5c4f3299faacd07a}},
      {"jeh6Qazxmdi57pa9S3XSnnZFIRrnc6s8QLrah5OX3SB/V2ErSPoEAumavzQPkdKF1/"
       "SfvmdL+qgF1C+Yawy562QaFqwVGq7+tW0yxP8FStb56ZRgNI4IOmI30s1Ei7iops9Uuw==",
       uint64_t{0x6a418974109c67b4}, uint64_t{0xfffe3bff0ae5e9bc}},
      {"6QO5nnDrY2/"
       "wrUXpltlKy2dSBcmK15fOY092CR7KxAjNfaY+"
       "aAmtWbbzQk3MjBg03x39afSUN1fkrWACdyQKRaGxgwq6MGNxI6W+8DLWJBHzIXrntrE/"
       "ml6fnNXEpxplWJ1vEs4=",
       uint64_t{0x8472f1c2b3d230a3}, uint64_t{0x1db785c0005166e4}},
      {"0oPxeEHhqhcFuwonNfLd5jF3RNATGZS6NPoS0WklnzyokbTqcl4BeBkMn07+fDQv83j/"
       "BpGUwcWO05f3+DYzocfnizpFjLJemFGsls3gxcBYxcbqWYev51tG3lN9EvRE+X9+Pwww",
       uint64_t{0x5e06068f884e73a7}, uint64_t{0xea000d962ad18418}},
      {"naSBSjtOKgAOg8XVbR5cHAW3Y+QL4Pb/JO9/"
       "oy6L08wvVRZqo0BrssMwhzBP401Um7A4ppAupbQeJFdMrysY34AuSSNvtNUy5VxjNECwiNt"
       "gwYHw7yakDUv8WvonctmnoSPKENegQg==",
       uint64_t{0x55290b1a8f170f59}, uint64_t{0xe42aef38359362d9}},
      {"vPyl8DxVeRe1OpilKb9KNwpGkQRtA94UpAHetNh+"
       "95V7nIW38v7PpzhnTWIml5kw3So1Si0TXtIUPIbsu32BNhoH7QwFvLM+"
       "JACgSpc5e3RjsL6Qwxxi11npwxRmRUqATDeMUfRAjxg=",
       uint64_t{0x5501cfd83dfe706a}, uint64_t{0xc8e95657348a3891}},
      {"QC9i2GjdTMuNC1xQJ74ngKfrlA4w3o58FhvNCltdIpuMhHP1YsDA78scQPLbZ3OCUgeQguY"
       "f/vw6zAaVKSgwtaykqg5ka/4vhz4hYqWU5ficdXqClHl+zkWEY26slCNYOM5nnDlly8Cj",
       uint64_t{0xe43ed13d13a66990}, uint64_t{0xc162eca864f238c6}},
      {"7CNIgQhAHX27nxI0HeB5oUTnTdgKpRDYDKwRcXfSFGP1XeT9nQF6WKCMjL1tBV6x7KuJ91G"
       "Zz11F4c+8s+MfqEAEpd4FHzamrMNjGcjCyrVtU6y+7HscMVzr7Q/"
       "ODLcPEFztFnwjvCjmHw==",
       uint64_t{0xdf43bc375cf5283f}, uint64_t{0xbe1fb373e20579ad}},
      {"Qa/hC2RPXhANSospe+gUaPfjdK/yhQvfm4cCV6/pdvCYWPv8p1kMtKOX3h5/"
       "8oZ31fsmx4Axphu5qXJokuhZKkBUJueuMpxRyXpwSWz2wELx5glxF7CM0Fn+"
       "OevnkhUn5jsPlG2r5jYlVn8=",
       uint64_t{0x8112b806d288d7b5}, uint64_t{0x628a1d4f40aa6ffd}},
      {"kUw/0z4l3a89jTwN5jpG0SHY5km/"
       "IVhTjgM5xCiPRLncg40aqWrJ5vcF891AOq5hEpSq0bUCJUMFXgct7kvnys905HjerV7Vs1G"
       "y84tgVJ70/2+pAZTsB/PzNOE/G6sOj4+GbTzkQu819OLB",
       uint64_t{0xd52a18abb001cb46}, uint64_t{0xa87bdb7456340f90}},
      {"VDdfSDbO8Tdj3T5W0XM3EI7iHh5xpIutiM6dvcJ/fhe23V/srFEkDy5iZf/"
       "VnA9kfi2C79ENnFnbOReeuZW1b3MUXB9lgC6U4pOTuC+"
       "jHK3Qnpyiqzj7h3ISJSuo2pob7vY6VHZo6Fn7exEqHg==",
       uint64_t{0xe12b76a2433a1236}, uint64_t{0x5960ef3ba982c801}},
      {"Ldfvy3ORdquM/R2fIkhH/ONi69mcP1AEJ6n/"
       "oropwecAsLJzQSgezSY8bEiEs0VnFTBBsW+RtZY6tDj03fnb3amNUOq1b7jbqyQkL9hpl+"
       "2Z2J8IaVSeownWl+bQcsR5/xRktIMckC5AtF4YHfU=",
       uint64_t{0x175bf7319cf1fa00}, uint64_t{0x5026586df9a431ec}},
      {"BrbNpb42+"
       "VzZAjJw6QLirXzhweCVRfwlczzZ0VX2xluskwBqyfnGovz5EuX79JJ31VNXa5hTkAyQat3l"
       "YKRADTdAdwE5PqM1N7YaMqqsqoAAAeuYVXuk5eWCykYmClNdSspegwgCuT+403JigBzi",
       uint64_t{0xd63d57b3f67525ae}, uint64_t{0xfe4b8a20fdf0840b}},
      {"gB3NGHJJvVcuPyF0ZSvHwnWSIfmaI7La24VMPQVoIIWF7Z74NltPZZpx2f+cocESM+"
       "ILzQW9p+BC8x5IWz7N4Str2WLGKMdgmaBfNkEhSHQDU0IJEOnpUt0HmjhFaBlx0/"
       "LTmhua+rQ6Wup8ezLwfg==",
       uint64_t{0x933faea858832b73}, uint64_t{0xdcb761867da7072f}},
      {"hTKHlRxx6Pl4gjG+6ksvvj0CWFicUg3WrPdSJypDpq91LUWRni2KF6+"
       "81ZoHBFhEBrCdogKqeK+hy9bLDnx7g6rAFUjtn1+cWzQ2YjiOpz4+"
       "ROBB7lnwjyTGWzJD1rXtlso1g2qVH8XJVigC5M9AIxM=",
       uint64_t{0x53d061e5f8e7c04f}, uint64_t{0xc10d4653667275b7}},
      {"IWQBelSQnhrr0F3BhUpXUIDauhX6f95Qp+A0diFXiUK7irwPG1oqBiqHyK/SH/"
       "9S+"
       "rln9DlFROAmeFdH0OCJi2tFm4afxYzJTFR4HnR4cG4x12JqHaZLQx6iiu6CE3rtWBVz99oA"
       "wCZUOEXIsLU24o2Y",
       uint64_t{0xdb4124556dd515e0}, uint64_t{0x727720deec13110b}},
      {"TKo+l+"
       "1dOXdLvIrFqeLaHdm0HZnbcdEgOoLVcGRiCbAMR0j5pIFw8D36tefckAS1RCFOH5IgP8yiF"
       "T0Gd0a2hI3+"
       "fTKA7iK96NekxWeoeqzJyctc6QsoiyBlkZerRxs5RplrxoeNg29kKDTM0K94mnhD9g==",
       uint64_t{0x4fb31a0dd681ee71}, uint64_t{0x710b009662858dc9}},
      {"YU4e7G6EfQYvxCFoCrrT0EFgVLHFfOWRTJQJ5gxM3G2b+"
       "1kJf9YPrpsxF6Xr6nYtS8reEEbDoZJYqnlk9lXSkVArm88Cqn6d25VCx3+"
       "49MqC0trIlXtb7SXUUhwpJK16T0hJUfPH7s5cMZXc6YmmbFuBNPE=",
       uint64_t{0x27cc72eefa138e4c}, uint64_t{0xfbf8f7a3ecac1eb7}},
      {"/I/"
       "eImMwPo1U6wekNFD1Jxjk9XQVi1D+"
       "FPdqcHifYXQuP5aScNQfxMAmaPR2XhuOQhADV5tTVbBKwCDCX4E3jcDNHzCiPvViZF1W27t"
       "xaf2BbFQdwKrNCmrtzcluBFYu0XZfc7RU1RmxK/RtnF1qHsq/O4pp",
       uint64_t{0x44bc2dfba4bd3ced}, uint64_t{0xb6fc4fcd0722e3df}},
      {"CJTT9WGcY2XykTdo8KodRIA29qsqY0iHzWZRjKHb9alwyJ7RZAE3V5Juv4MY3MeYEr1EPCC"
       "MxO7yFXqT8XA8YTjaMp3bafRt17Pw8JC4iKJ1zN+WWKOESrj+"
       "3aluGQqn8z1EzqY4PH7rLG575PYeWsP98BugdA==",
       uint64_t{0x242da1e3a439bed8}, uint64_t{0x7cb86dcc55104aac}},
      {"ZlhyQwLhXQyIUEnMH/"
       "AEW27vh9xrbNKJxpWGtrEmKhd+nFqAfbeNBQjW0SfG1YI0xQkQMHXjuTt4P/"
       "EpZRtA47ibZDVS8TtaxwyBjuIDwqcN09eCtpC+Ls+"
       "vWDTLmBeDM3u4hmzz4DQAYsLiZYSJcldg9Q3wszw=",
       uint64_t{0xdc559c746e35c139}, uint64_t{0x19e71e9b45c3a51e}},
      {"v2KU8y0sCrBghmnm8lzGJlwo6D6ObccAxCf10heoDtYLosk4ztTpLlpSFEyu23MLA1tJkcg"
       "Rko04h19QMG0mOw/"
       "wc93EXAweriBqXfvdaP85sZABwiKO+6rtS9pacRVpYYhHJeVTQ5NzrvBvi1huxAr+"
       "xswhVMfL",
       uint64_t{0xd0b0350275b9989}, uint64_t{0x51de38573c2bea48}},
      {"QhKlnIS6BuVCTQsnoE67E/"
       "yrgogE8EwO7xLaEGei26m0gEU4OksefJgppDh3X0x0Cs78Dr9IHK5b977CmZlrTRmwhlP8p"
       "M+UzXPNRNIZuN3ntOum/QhUWP8SGpirheXENWsXMQ/"
       "nxtxakyEtrNkKk471Oov9juP8oQ==",
       uint64_t{0xb04489e41d17730c}, uint64_t{0xa73ab6996d6df158}},
      {"/ZRMgnoRt+Uo6fUPr9FqQvKX7syhgVqWu+"
       "WUSsiQ68UlN0efSP6Eced5gJZL6tg9gcYJIkhjuQNITU0Q3TjVAnAcobgbJikCn6qZ6pRxK"
       "BY4MTiAlfGD3T7R7hwJwx554MAy++Zb/YUFlnCaCJiwQMnowF7aQzwYFCo=",
       uint64_t{0x2217285eb4572156}, uint64_t{0x55ef2b8c930817b2}},
      {"NB7tU5fNE8nI+SXGfipc7sRkhnSkUF1krjeo6k+8FITaAtdyz+"
       "o7mONgXmGLulBPH9bEwyYhKNVY0L+njNQrZ9YC2aXsFD3PdZsxAFaBT3VXEzh+"
       "NGBTjDASNL3mXyS8Yv1iThGfHoY7T4aR0NYGJ+k+pR6f+KrPC96M",
       uint64_t{0x12c2e8e68aede73b}, uint64_t{0xb2850bf5fae87157}},
      {"8T6wrqCtEO6/rwxF6lvMeyuigVOLwPipX/FULvwyu+1wa5sQGav/"
       "2FsLHUVn6cGSi0LlFwLewGHPFJDLR0u4t7ZUyM//"
       "x6da0sWgOa5hzDqjsVGmjxEHXiaXKW3i4iSZNuxoNbMQkIbVML+"
       "DkYu9ND0O2swg4itGeVSzXA==",
       uint64_t{0x4d612125bdc4fd00}, uint64_t{0xecf3de1acd04651f}},
      {"Ntf1bMRdondtMv1CYr3G80iDJ4WSAlKy5H34XdGruQiCrnRGDBa+"
       "eUi7vKp4gp3BBcVGl8eYSasVQQjn7MLvb3BjtXx6c/"
       "bCL7JtpzQKaDnPr9GWRxpBXVxKREgMM7d8lm35EODv0w+"
       "hQLfVSh8OGs7fsBb68nNWPLeeSOo=",
       uint64_t{0x81826b553954464e}, uint64_t{0xcc0a40552559ff32}},
      {"VsSAw72Ro6xks02kaiLuiTEIWBC5bgqr4WDnmP8vglXzAhixk7td926rm9jNimL+"
       "kroPSygZ9gl63aF5DCPOACXmsbmhDrAQuUzoh9ZKhWgElLQsrqo1KIjWoZT5b5QfVUXY9lS"
       "IBg3U75SqORoTPq7HalxxoIT5diWOcJQi",
       uint64_t{0xc2e5d345dc0ddd2d}, uint64_t{0xc385c374f20315b1}},
      {"j+loZ+C87+"
       "bJxNVebg94gU0mSLeDulcHs84tQT7BZM2rzDSLiCNxUedHr1ZWJ9ejTiBa0dqy2I2ABc++"
       "xzOLcv+//YfibtjKtYggC6/3rv0XCc7xu6d/"
       "O6xO+XOBhOWAQ+IHJVHf7wZnDxIXB8AUHsnjEISKj7823biqXjyP3g==",
       uint64_t{0x3da6830a9e32631e}, uint64_t{0xb90208a4c7234183}},
      {"f3LlpcPElMkspNtDq5xXyWU62erEaKn7RWKlo540gR6mZsNpK1czV/"
       "sOmqaq8XAQLEn68LKj6/"
       "cFkJukxRzCa4OF1a7cCAXYFp9+wZDu0bw4y63qbpjhdCl8GO6Z2lkcXy7KOzbPE01ukg7+"
       "gN+7uKpoohgAhIwpAKQXmX5xtd0=",
       uint64_t{0xc9ae5c8759b4877a}, uint64_t{0x58aa1ca7a4c075d9}},
  };

  for (const auto& expected_result : expected_results) {
    std::string str;
    ASSERT_TRUE(absl::Base64Unescape(expected_result.base64_data, &str));
    EXPECT_EQ(absl::hash_internal::Wyhash(str.data(), str.size(),
                                          expected_result.seed, kSalt),
              expected_result.hash);
  }
}

}  // namespace
