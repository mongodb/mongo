// Copyright 2022 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "./fuzztest/internal/serialization.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <type_traits>
#include <variant>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "./fuzztest/internal/test_protobuf.pb.h"
#include "google/protobuf/text_format.h"
#include "google/protobuf/util/message_differencer.h"

namespace fuzztest::internal {
namespace {

using ::testing::_;
using ::testing::AllOf;
using ::testing::ElementsAre;
using ::testing::FieldsAre;
using ::testing::NanSensitiveDoubleEq;
using ::testing::Not;
using ::testing::Optional;
using ::testing::Pair;
using ::testing::StartsWith;
using ::testing::VariantWith;

template <typename T>
auto ValueIs(const T& v) {
  if constexpr (std::is_same_v<T, double>) {
    return FieldsAre(VariantWith<double>(NanSensitiveDoubleEq(v)));
  } else {
    return FieldsAre(VariantWith<T>(v));
  }
}

template <typename... T>
auto SubsAre(const T&... v) {
  return FieldsAre(VariantWith<std::vector<IRObject>>(ElementsAre(v...)));
}

struct VerifyVisitor {
  const IRObjectTestProto& proto;

  void operator()(uint64_t v) const {
    EXPECT_EQ(v, proto.i());
    EXPECT_EQ(proto.sub_size(), 0);
  }
  void operator()(double v) const {
    EXPECT_THAT(v, NanSensitiveDoubleEq(proto.d()));
    EXPECT_EQ(proto.sub_size(), 0);
  }
  void operator()(const std::string& v) const {
    EXPECT_EQ(v, proto.s());
    EXPECT_EQ(proto.sub_size(), 0);
  }
  void operator()(const std::vector<IRObject>& subs) const {
    EXPECT_EQ(0, proto.value_case());
    ASSERT_EQ(subs.size(), proto.sub_size());
    for (int i = 0; i < subs.size(); ++i) {
      std::visit(VerifyVisitor{proto.sub(i)}, subs[i].value);
    }
  }

  void operator()(std::monostate) const {
    EXPECT_EQ(0, proto.value_case());
    EXPECT_EQ(proto.sub_size(), 0);
  }
};

TEST(SerializerTest, HeaderIsCorrect) {
  EXPECT_THAT(IRObject(0).ToString(/*binary_format=*/false),
              AllOf(StartsWith("FUZZTESTv1"), Not(StartsWith("FUZZTESTv1b"))));
  EXPECT_THAT(IRObject(0).ToString(/*binary_format=*/true),
              StartsWith("FUZZTESTv1b"));
}

// We manually write the serialized form to test the error handling of the
// parser. The serializer would not generate these, so we can't use it.
TEST(SerializerTest, WrongHeaderWontParse) {
  EXPECT_THAT(IRObject::FromString("FUZZTESTv2"), Not(Optional(_)));
  EXPECT_THAT(IRObject::FromString("FUZZtESTv1"), Not(Optional(_)));
  EXPECT_THAT(IRObject::FromString("-FUZZTESTv1"), Not(Optional(_)));
}

TEST(SerializerTest, SubsAccesors) {
  IRObject obj;
  // The empty obj shows as an empty subs too due to how it is serialized.
  EXPECT_THAT(obj.Subs(), Optional(ElementsAre()));

  auto& subs = obj.MutableSubs();
  EXPECT_THAT(obj.Subs(), Optional(ElementsAre()));

  subs.emplace_back(17);
  subs.emplace_back("ABC");
  EXPECT_THAT(obj.Subs(), Optional(ElementsAre(ValueIs<uint64_t>(17),
                                               ValueIs<std::string>("ABC"))));

  // Another call keeps them.
  obj.MutableSubs();
  EXPECT_THAT(obj.Subs(), Optional(ElementsAre(ValueIs<uint64_t>(17),
                                               ValueIs<std::string>("ABC"))));
}

IRObject CreateRecursiveObject(int depth) {
  IRObject obj;
  if (depth > 0) {
    obj.MutableSubs().push_back(CreateRecursiveObject(depth - 1));
  }
  return obj;
}

TEST(SerializerTest, RecursiveStructureBelowDepthLimitGetsParsed) {
  EXPECT_TRUE(IRObject::FromString(CreateRecursiveObject(/*depth=*/100)
                                       .ToString(/*binary_format=*/false))
                  .has_value());
  EXPECT_TRUE(
      IRObject::FromString(
          CreateRecursiveObject(/*depth=*/100).ToString(/*binary_format=*/true))
          .has_value());
}

TEST(SerializerTest, RecursiveStructureAboveDepthLimitDoesNotGetParsed) {
  EXPECT_FALSE(IRObject::FromString(CreateRecursiveObject(/*depth=*/150)
                                        .ToString(/*binary_format=*/false))
                   .has_value());
  EXPECT_FALSE(
      IRObject::FromString(
          CreateRecursiveObject(/*depth=*/150).ToString(/*binary_format=*/true))
          .has_value());
}

struct SerializerRoundTripParam {
  bool binary_format;
};

class SerializerRoundTripTest
    : public testing::TestWithParam<SerializerRoundTripParam> {};

void VerifyProtobufFormat(const IRObject& object) {
  IRObjectTestProto proto;
  std::string s = object.ToString(/*binary_format=*/false);
  // Chop the header.
  s.erase(0, strlen("FUZZTESTv1\n"));

  ASSERT_TRUE(google::protobuf::TextFormat::ParseFromString(s, &proto));
  std::visit(VerifyVisitor{proto}, object.value);
}

template <typename... T>
void RoundTripVerify(bool binary_format, const T&... values) {
  IRObject object;
  object.value = std::vector{IRObject{values}...};
  std::string s = object.ToString(binary_format);

  SCOPED_TRACE(s);

  if (!binary_format) VerifyProtobufFormat(object);

  EXPECT_THAT(IRObject::FromString(s),
              Optional(SubsAre(ValueIs<T>(values)...)));
}

template <typename T>
using L = std::numeric_limits<T>;

TEST_P(SerializerRoundTripTest, ScalarsRoundTrip) {
  using S = std::string;
  RoundTripVerify(GetParam().binary_format, uint64_t{0}, L<uint64_t>::min(),
                  L<uint64_t>::max(),                                        //
                  double{1.2}, L<double>::max(), L<double>::min(),           //
                  L<double>::lowest(), L<double>::infinity(), std::nan(""),  //
                  S(""), S("A"), S("\nSpecial\r Chars\"\n12\\"),
                  S("\0Zero\0", 6));
}

TEST_P(SerializerRoundTripTest, SubobjectsRoundTrip) {
  IRObject root{std::vector{
      IRObject{"child1"}, IRObject{"child2"},
      IRObject{std::vector<IRObject>{
          IRObject{"child3.1"}, IRObject{std::vector{IRObject{"child3.2.1"},
                                                     IRObject{"child3.2.2"}}}}},
      IRObject{"child4"}}};

  std::string s = root.ToString(GetParam().binary_format);

  SCOPED_TRACE(s);

  if (!GetParam().binary_format) VerifyProtobufFormat(root);

  std::optional<IRObject> obj = IRObject::FromString(s);
  EXPECT_THAT(
      obj, Optional(SubsAre(
               ValueIs<std::string>("child1"), ValueIs<std::string>("child2"),
               SubsAre(ValueIs<std::string>("child3.1"),
                       SubsAre(ValueIs<std::string>("child3.2.1"),
                               ValueIs<std::string>("child3.2.2"))),
               ValueIs<std::string>("child4"))));
}

TEST_P(SerializerRoundTripTest, EmptyObjectRoundTrips) {
  std::string s = IRObject{}.ToString(GetParam().binary_format);
  SCOPED_TRACE(s);
  EXPECT_THAT(IRObject::FromString(s), Optional(ValueIs<std::monostate>({})));
}

template <typename T>
void TestScalarRoundTrips(bool binary_format, T value) {
  EXPECT_THAT(IRObject(value).GetScalar<T>(), Optional(value));

  IRObject obj;
  obj.SetScalar(value);
  EXPECT_THAT(obj.GetScalar<T>(), Optional(value));

  auto roundtrip = IRObject::FromString(obj.ToString(binary_format));
  EXPECT_THAT(obj.GetScalar<T>(), Optional(value));
}

TEST_P(SerializerRoundTripTest, ScalarConversionsWorks) {
  TestScalarRoundTrips(GetParam().binary_format, true);
  TestScalarRoundTrips(GetParam().binary_format, 'a');
  TestScalarRoundTrips(GetParam().binary_format, -1);
  TestScalarRoundTrips(GetParam().binary_format, size_t{123});
  TestScalarRoundTrips(GetParam().binary_format, int64_t{-1});
  TestScalarRoundTrips(GetParam().binary_format, -123LL);
  TestScalarRoundTrips(GetParam().binary_format, 1.5f);
  TestScalarRoundTrips(GetParam().binary_format, std::string("ABC"));
  enum E { kEnum = 18 };
  enum class E2 { kEnum = 18 };
  TestScalarRoundTrips(GetParam().binary_format, E::kEnum);
  TestScalarRoundTrips(GetParam().binary_format, E2::kEnum);
}

INSTANTIATE_TEST_SUITE_P(
    SerializerRoundTripTest, SerializerRoundTripTest,
    testing::Values(SerializerRoundTripParam{/*binary_format=*/false},
                    SerializerRoundTripParam{/*binary_format=*/true}));

TEST(TextFormatSerializerTest, IndentationIsCorrect) {
  // This test checks the actual returned string to verify the indentation.
  // The indentation is irrelevant for the correctness of the algorithm, but it
  // is good for human readability.

  IRObject root{
      std::vector{IRObject{uint64_t{1}}, IRObject{uint64_t{2}},
                  IRObject{std::vector<IRObject>{
                      IRObject{uint64_t{31}},
                      IRObject{std::vector{IRObject{uint64_t{321}},
                                           IRObject{uint64_t{322}}}}}},
                  IRObject{uint64_t{4}}}};

  std::string s = root.ToString(/*binary_format=*/false);

  EXPECT_EQ(s, R"(FUZZTESTv1
sub { i: 1 }
sub { i: 2 }
sub {
  sub { i: 31 }
  sub {
    sub { i: 321 }
    sub { i: 322 }
  }
}
sub { i: 4 }
)");
}

TEST(TextFormatSerializerTest, HandlesUnterminatedString) {
  EXPECT_THAT(IRObject::FromString("FUZZTESTv1\""), Not(Optional(_)));
}

TEST(TextFormatSerializerTest, BadScalarWontParse) {
  EXPECT_THAT(IRObject::FromString("FUZZTESTv1 i: 1"),
              Optional(ValueIs<uint64_t>(1)));
  // Out of bounds values
  EXPECT_THAT(IRObject::FromString("FUZZTESTv1 i: 123456789012345678901"),
              Not(Optional(_)));
  EXPECT_THAT(IRObject::FromString("FUZZTESTv1 i: -1"), Not(Optional(_)));
  // Missing :
  EXPECT_THAT(IRObject::FromString("FUZZTESTv1 i 1"), Not(Optional(_)));
  // Bad tag
  EXPECT_THAT(IRObject::FromString("FUZZTESTv1 x: 1"), Not(Optional(_)));
  // Wrong separator
  EXPECT_THAT(IRObject::FromString("FUZZTESTv1 i; 1"), Not(Optional(_)));
  // Extra close
  EXPECT_THAT(IRObject::FromString("FUZZTESTv1 i: 1}"), Not(Optional(_)));
}

TEST(TextFormatSerializerTest, BadSubWontParse) {
  EXPECT_THAT(IRObject::FromString("FUZZTESTv1 sub { i: 0 }"),
              Optional(SubsAre(ValueIs<uint64_t>(0))));
  EXPECT_THAT(IRObject::FromString("FUZZTESTv1 sub: { }"), Not(Optional(_)));
  EXPECT_THAT(IRObject::FromString("FUZZTESTv1 sub  }"), Not(Optional(_)));
  EXPECT_THAT(IRObject::FromString("FUZZTESTv1 sub { "), Not(Optional(_)));
  EXPECT_THAT(IRObject::FromString("FUZZTESTv1 sub { } }"), Not(Optional(_)));
}

TEST(TextFormatSerializerTest, ExtraWhitespaceIsFine) {
  EXPECT_THAT(IRObject::FromString("FUZZTESTv1 i: 0 \n "),
              Optional(ValueIs<uint64_t>(0)));
  EXPECT_THAT(IRObject::FromString("FUZZTESTv1 sub {   \n i:   0 \n}  \n "),
              Optional(SubsAre(ValueIs<uint64_t>(0))));
}

TEST(BinaryFormatSerializerTest, MalformedObjectSizeIsRejectedWithoutOOM) {
  static constexpr char kGoodInput[] =
      "FUZZTESTv1b\x04\x01\x00\x00\x00\x00\x00\x00\x00\x00";
  static constexpr char kBadInput[] =
      "FUZZTESTv1b\x04\xff\xff\xff\xff\xff\xff\xff\xff\x00";
  EXPECT_THAT(IRObject::FromString({kGoodInput, sizeof(kGoodInput) - 1}),
              Optional(SubsAre(ValueIs<std::monostate>({}))));
  // Expect grace failure instead of OOM error.
  EXPECT_EQ(IRObject::FromString({kBadInput, sizeof(kBadInput) - 1}),
            std::nullopt);
}

TEST(IRToCorpus, SpecializationIsBackwardCompatible) {
  EXPECT_THAT(
      (IRObject{std::vector<IRObject>{IRObject{1}, IRObject{2}, IRObject{3}}}
           .ToCorpus<std::vector<int8_t>>()),
      Optional(ElementsAre(1, 2, 3)));
}

TEST(CorpusToIR, Specialization) {
  EXPECT_TRUE(IRObject::FromCorpus(std::vector<int8_t>{1, 2, 3})
                  .GetScalar<std::string>()
                  .has_value());
  EXPECT_TRUE(IRObject::FromCorpus(std::vector<uint8_t>{1, 2, 3})
                  .GetScalar<std::string>()
                  .has_value());
  EXPECT_TRUE(
      IRObject::FromCorpus(
          std::vector<std::byte>{std::byte{1}, std::byte{2}, std::byte{3}})
          .GetScalar<std::string>()
          .has_value());
}

TEST(CorpusToIR, ValidRoundTrips) {
  const auto round_trip = [](auto v) -> std::optional<decltype(v)> {
    return IRObject::FromCorpus(v).template ToCorpus<decltype(v)>();
  };

  // Monostates
  EXPECT_THAT(round_trip(std::true_type{}), Optional(std::true_type{}));
  EXPECT_THAT(round_trip(std::false_type{}), Optional(std::false_type{}));

  // Scalars
  EXPECT_THAT(round_trip('a'), Optional('a'));
  EXPECT_THAT(round_trip(true), Optional(true));
  EXPECT_THAT(round_trip(false), Optional(false));
  EXPECT_THAT(round_trip(-1), Optional(-1));
  EXPECT_THAT(round_trip(size_t{123}), Optional(size_t{123}));
  EXPECT_THAT(round_trip(1.5f), Optional(1.5f));
  EXPECT_THAT(round_trip(1234.), Optional(1234.));
  EXPECT_THAT(round_trip(std::string("ABC")), Optional(std::string("ABC")));
  enum E { kEnum };
  enum class E2 { kEnum };
  EXPECT_THAT(round_trip(E::kEnum), Optional(E::kEnum));
  EXPECT_THAT(round_trip(E2::kEnum), Optional(E2::kEnum));

  // Specializations
  EXPECT_THAT(round_trip(std::vector<int8_t>{1, 2, 3}),
              Optional(ElementsAre(1, 2, 3)));
  EXPECT_THAT(round_trip(std::vector<uint8_t>{1, 2, 3}),
              Optional(ElementsAre(1, 2, 3)));
  EXPECT_THAT(round_trip(std::vector<std::byte>{std::byte{1}, std::byte{2},
                                                std::byte{3}}),
              Optional(ElementsAre(std::byte{1}, std::byte{2}, std::byte{3})));

  // Compound types
  EXPECT_THAT(round_trip(std::vector<bool>{true, false, true, true}),
              Optional(ElementsAre(true, false, true, true)));
  EXPECT_THAT(round_trip(std::vector{1, 2, 3}), Optional(ElementsAre(1, 2, 3)));
  EXPECT_THAT(round_trip(std::vector{std::string("A"), std::string("B")}),
              Optional(ElementsAre("A", "B")));
  EXPECT_THAT(round_trip(std::tuple(1, std::string("A"), 1.4)),
              Optional(FieldsAre(1, "A", 1.4)));
  EXPECT_THAT(round_trip(std::vector{std::tuple(1, 2)}),
              Optional(ElementsAre(FieldsAre(1, 2))));
  EXPECT_THAT(round_trip(std::array<int, 3>{0, 2, 4}),
              Optional(ElementsAre(0, 2, 4)));
  EXPECT_THAT(round_trip(std::variant<int, std::string>(1000)),
              Optional(VariantWith<int>(1000)));
  EXPECT_THAT(round_trip(std::variant<int, std::string>("ABC")),
              Optional(VariantWith<std::string>("ABC")));
  EXPECT_THAT(round_trip(std::map<int, int>{{1, 2}, {3, 4}}),
              Optional(ElementsAre(Pair(1, 2), Pair(3, 4))));

  // Proto
  TestProtobuf proto;
  proto.set_b(true);
  const std::optional<TestProtobuf> round_trip_proto = round_trip(proto);
  EXPECT_TRUE(
      round_trip_proto.has_value() &&
      google::protobuf::util::MessageDifferencer::Equals(*round_trip_proto, proto));

  // IRObject's identity
  IRObject obj(1979);
  obj = round_trip(obj).value();
  EXPECT_THAT(obj.GetScalar<int>(), Optional(1979));
  obj.MutableSubs().emplace_back("ABC");
  obj = round_trip(obj).value();
  EXPECT_THAT(
      obj.Subs(),
      Optional(ElementsAre(FieldsAre(VariantWith<std::string>("ABC")))));
}

TEST(CorpusToIR, FailureConditions) {
  // simple
  EXPECT_FALSE(IRObject(1).ToCorpus<std::true_type>());
  EXPECT_FALSE(IRObject("ABC").ToCorpus<int>());

  // variant
  {
    using V = std::variant<int, std::string>;
    // Valid index, but bad value.
    IRObject var = IRObject::FromCorpus(V(2));
    auto& v = std::get<std::vector<IRObject>>(var.value);
    EXPECT_THAT(v[0].GetScalar<int>(), Optional(0));
    v[0] = IRObject(1);
    EXPECT_FALSE(var.ToCorpus<V>());

    // Reset
    v[0] = IRObject(0);
    EXPECT_THAT(var.ToCorpus<V>(), Optional(VariantWith<int>(2)));

    // Invalid index
    v[0] = IRObject(2);
    EXPECT_FALSE(var.ToCorpus<V>());
  }

  // proto
  EXPECT_FALSE(IRObject(1).ToCorpus<TestProtobuf>());
  EXPECT_TRUE(IRObject("").ToCorpus<TestProtobuf>());

  // container
  using Vector = std::vector<int>;
  EXPECT_FALSE(IRObject(1).ToCorpus<Vector>());
  EXPECT_TRUE(IRObject(std::vector<IRObject>{IRObject(1), IRObject(2)})
                  .ToCorpus<Vector>());
  EXPECT_FALSE(IRObject(std::vector<IRObject>{IRObject(1), IRObject("ABC")})
                   .ToCorpus<Vector>());

  // tuples
  using Tuple = std::tuple<int, std::string>;
  EXPECT_FALSE(IRObject(1).ToCorpus<Tuple>());
  EXPECT_TRUE(IRObject(std::vector<IRObject>{IRObject(1), IRObject("ABC")})
                  .ToCorpus<Tuple>());
  EXPECT_FALSE(IRObject(std::vector<IRObject>{IRObject("A"), IRObject("ABC")})
                   .ToCorpus<Tuple>());
}

// TODO(sbenzaquen): Add tests for failing conditions in the IR->Corpus conversion.

}  // namespace
}  // namespace fuzztest::internal
