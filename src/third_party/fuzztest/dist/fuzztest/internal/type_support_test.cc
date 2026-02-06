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

#include "./fuzztest/internal/type_support.h"

#include <array>
#include <cmath>
#include <complex>
#include <cstdint>
#include <limits>
#include <list>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/numeric/int128.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/string_view.h"
#include "absl/strings/strip.h"
#include "absl/time/time.h"
#include "./fuzztest/domain.h"
#include "./fuzztest/internal/meta.h"
#include "./fuzztest/internal/printer.h"
#include "./fuzztest/internal/test_protobuf.pb.h"
#include "google/protobuf/text_format.h"

namespace fuzztest::internal {
namespace {

using ::fuzztest::domain_implementor::PrintMode;
using ::fuzztest::domain_implementor::PrintValue;
using ::testing::AllOf;
using ::testing::Contains;
using ::testing::Each;
using ::testing::ElementsAre;
using ::testing::EndsWith;
using ::testing::HasSubstr;
using ::testing::Le;
using ::testing::MatchesRegex;

template <typename Domain>
std::vector<std::string> TestPrintValue(const corpus_type_t<Domain>& value,
                                        const Domain& domain) {
  std::vector<std::string> res(2);
  PrintValue(domain, value, &res[0], PrintMode::kHumanReadable);
  PrintValue(domain, value, &res[1], PrintMode::kSourceCode);
  return res;
}

template <typename T>
std::vector<std::string> TestPrintValue(const T& value) {
  std::vector<std::string> res(2);
  auto traits = AutodetectTypePrinter<T>();
  traits.PrintUserValue(value, &res[0], PrintMode::kHumanReadable);
  traits.PrintUserValue(value, &res[1], PrintMode::kSourceCode);
  return res;
}

TEST(BoolTest, Printer) {
  EXPECT_THAT(TestPrintValue(false), Each("false"));
  EXPECT_THAT(TestPrintValue(true), Each("true"));
}

TEST(CharTest, Printer) {
  EXPECT_THAT(TestPrintValue('a'), ElementsAre("'a' (97)", "'a'"));
  EXPECT_THAT(TestPrintValue(static_cast<char>(200)),
              ElementsAre("0xc8 (200)", "'\\310'"));
  EXPECT_THAT(TestPrintValue(static_cast<char>(1)),
              ElementsAre("0x01 (1)", "'\\001'"));
}

template <typename T>
class IntegralTest : public testing::Test {};

using IntegralTypes = testing::Types<signed char, unsigned char,     //
                                     short, unsigned short,          //
                                     int, unsigned int,              //
                                     long, unsigned long,            //
                                     long long, unsigned long long,  //
                                     absl::int128, absl::uint128>;

TYPED_TEST_SUITE(IntegralTest, IntegralTypes, );

TYPED_TEST(IntegralTest, Printer) {
  for (auto v : {TypeParam{0}, std::numeric_limits<TypeParam>::min(),
                 std::numeric_limits<TypeParam>::max()}) {
    if constexpr (std::is_integral_v<TypeParam>) {
      EXPECT_THAT(TestPrintValue(v), Each(std::to_string(v)));
    } else {
      // Once the absl stable version supports [u]int128, and also
      // have same output for chat streamline the implementation and use only
      // StrFormat for all types.
      std::stringstream ss;
      ss << v;
      EXPECT_THAT(TestPrintValue(v), Each(ss.str()));
    }
  }
}
enum Color { kRed, kBlue, kGreen };
enum ColorChar : char { kRedChar = 'r', kBlueChar = 'b' };
enum class ColorClass { kRed, kBlue, kGreen };
enum class ColorClassChar : char { kRed = 'r', kBlue = 'b' };
TEST(EnumTest, Printer) {
  EXPECT_THAT(TestPrintValue(kRed),
              ElementsAre("Color{0}", "static_cast<Color>(0)"));
  EXPECT_THAT(
      TestPrintValue(kRedChar),
      ElementsAre("ColorChar{'r' (114)}", "static_cast<ColorChar>('r')"));
  EXPECT_THAT(TestPrintValue(ColorClass::kRed),
              ElementsAre("ColorClass{0}", "static_cast<ColorClass>(0)"));
  EXPECT_THAT(TestPrintValue(ColorClassChar::kRed),
              ElementsAre("ColorClassChar{'r' (114)}",
                          "static_cast<ColorClassChar>('r')"));
}

template <typename T>
class FloatingTest : public testing::Test {};

using FloatingTypes = testing::Types<float, double, long double>;

TYPED_TEST_SUITE(FloatingTest, FloatingTypes, );

TYPED_TEST(FloatingTest, Printer) {
  absl::string_view suffix = std::is_same_v<float, TypeParam>    ? "f"
                             : std::is_same_v<double, TypeParam> ? ""
                                                                 : "L";
  EXPECT_THAT(TestPrintValue(TypeParam{0}), Each(absl::StrCat("0.", suffix)));
  for (auto v : {std::numeric_limits<TypeParam>::min(),
                 std::numeric_limits<TypeParam>::max()}) {
    EXPECT_THAT(TestPrintValue(v),
                Each(AllOf(HasSubstr("e"), Contains('.').Times(Le(1)),
                           EndsWith(suffix))));
  }
  TypeParam inf = std::numeric_limits<TypeParam>::infinity();
  auto type = GetTypeName<TypeParam>();
  EXPECT_THAT(TestPrintValue(inf),
              ElementsAre(absl::StrFormat("%f", inf),
                          absl::StrFormat("std::numeric_limits<%s>::infinity()",
                                          type)));
  EXPECT_THAT(TestPrintValue(-inf),
              ElementsAre(absl::StrFormat("%f", -inf),
                          absl::StrFormat(
                              "-std::numeric_limits<%s>::infinity()", type)));
  TypeParam nan = std::nan("");
  EXPECT_THAT(
      TestPrintValue(nan),
      ElementsAre(absl::StrFormat("%f", nan),
                  std::is_same_v<float, TypeParam>    ? "std::nanf(\"\")"
                  : std::is_same_v<double, TypeParam> ? "std::nan(\"\")"
                                                      : "std::nanl(\"\")"));

  // Check round tripping.
  for (auto v : {TypeParam{0.0013660046866830892},
                 std::numeric_limits<TypeParam>::epsilon()}) {
    auto printed_v = TestPrintValue(v);
    std::stringstream human_v_str;
    std::stringstream source_code_v_str;
    human_v_str << absl::StripSuffix(printed_v[0], suffix);
    source_code_v_str << absl::StripSuffix(printed_v[1], suffix);
    TypeParam human_v = TypeParam{0};
    TypeParam source_code_v = TypeParam{0};
    ASSERT_TRUE(human_v_str >> human_v);
    ASSERT_TRUE(source_code_v_str >> source_code_v);
    EXPECT_EQ(v, human_v);
    EXPECT_EQ(v, source_code_v);
  }
}

TEST(StringTest, Printer) {
  EXPECT_THAT(TestPrintValue(std::string("ABC")), Each("\"ABC\""));
  EXPECT_THAT(
      TestPrintValue(std::string{'\0', 'a', '\223', 'b'}),
      ElementsAre(R"("\000a\223b")", R"(std::string("\000a\223b", 4))"));
  EXPECT_THAT(TestPrintValue(std::string("printf(\"Hello, world!\");")),
              ElementsAre(R"("printf("Hello, world!");")",
                          R"("printf(\"Hello, world!\");")"));
}

TEST(ByteArrayTest, Printer) {
  EXPECT_THAT(
      TestPrintValue(std::vector<uint8_t>{'\0', 'a', 0223, 'b', '\"'}),
      ElementsAre(R"("\000a\223b"")",
                  R"(fuzztest::ToByteArray(std::string("\000a\223b\"", 5)))"));
  EXPECT_EQ(std::string("\000a\223b\"", 5).size(), 5);
}

struct UserDefinedWithAbslStringify {
  std::string foo;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const UserDefinedWithAbslStringify& v) {
    absl::Format(&sink, "{foo=\"%s\"}", v.foo);
  }
};

TEST(CompoundTest, Printer) {
  EXPECT_THAT(
      TestPrintValue(std::pair(1, 1.5), Arbitrary<std::pair<int, double>>()),
      Each("{1, 1.5}"));
  EXPECT_THAT(TestPrintValue(std::tuple(2, -3, -0.0),
                             Arbitrary<std::tuple<int, int, double>>()),
              Each("{2, -3, -0.}"));

  struct UserDefined {
    int i;
    double d;
    std::string s;
  };
  EXPECT_THAT(TestPrintValue(
                  std::tuple{2, -3.5, "Foo"},
                  StructOf<UserDefined>(Arbitrary<int>(), Arbitrary<double>(),
                                        Arbitrary<std::string>())),
              Each("UserDefined{2, -3.5, \"Foo\"}"));
  EXPECT_THAT(
      TestPrintValue(std::tuple{"Foo"}, StructOf<UserDefinedWithAbslStringify>(
                                            Arbitrary<std::string>())),
      ElementsAre("{foo=\"Foo\"}", "UserDefinedWithAbslStringify{\"Foo\"}"));
}

TEST(ProtobufTest, Printer) {
  internal::TestProtobuf proto;
  proto.set_b(true);
  proto.add_rep_subproto()->set_subproto_i32(17);
  std::string proto_text;
  ASSERT_TRUE(google::protobuf::TextFormat::PrintToString(proto, &proto_text));
  EXPECT_THAT(
      TestPrintValue(proto),
      ElementsAre(
          MatchesRegex(absl::StrCat("\\(.*\n?", proto_text, "\\)")),
          MatchesRegex(absl::StrCat(R"re(.*ParseTe[sx]tProto.*\(R"pb\(.*\n?)re",
                                    proto_text, R"re(\)pb"\))re"))));
}

TEST(ProtobufEnumTest, Printer) {
  auto domain = Arbitrary<internal::TestProtobuf_Enum>();
  EXPECT_THAT(TestPrintValue(TestProtobuf_Enum_Label2, domain),
              ElementsAre("fuzztest::internal::TestProtobuf::Label2 (1)",
                          "fuzztest::internal::TestProtobuf::Label2"));

  domain = Arbitrary<internal::TestProtobuf::Enum>();
  EXPECT_THAT(TestPrintValue(TestProtobuf::Label3, domain),
              ElementsAre("fuzztest::internal::TestProtobuf::Label3 (2)",
                          "fuzztest::internal::TestProtobuf::Label3"));

  EXPECT_THAT(
      TestPrintValue(static_cast<internal::TestProtobuf_Enum>(100), domain),
      ElementsAre("fuzztest::internal::TestProtobuf_Enum{100}",
                  "static_cast<fuzztest::internal::TestProtobuf_Enum>(100)"));

  auto bare_domain = Arbitrary<internal::BareEnum>();
  EXPECT_THAT(TestPrintValue(internal::BareEnum::LABEL_OTHER, bare_domain),
              ElementsAre("fuzztest::internal::BareEnum::LABEL_OTHER (10)",
                          "fuzztest::internal::BareEnum::LABEL_OTHER"));

  auto element_domain = ElementOf(
      {internal::TestProtobuf::Label3, internal::TestProtobuf::Label4});
  using corpus_type = corpus_type_t<decltype(element_domain)>;
  EXPECT_THAT(TestPrintValue(corpus_type(0), element_domain),
              ElementsAre("fuzztest::internal::TestProtobuf::Label3 (2)",
                          "fuzztest::internal::TestProtobuf::Label3"));

  auto bare_element_domain = ElementOf({internal::BareEnum::LABEL_OTHER});
  using corpus_type = corpus_type_t<decltype(bare_element_domain)>;
  EXPECT_THAT(TestPrintValue(corpus_type(0), bare_element_domain),
              ElementsAre("fuzztest::internal::BareEnum::LABEL_OTHER (10)",
                          "fuzztest::internal::BareEnum::LABEL_OTHER"));
}

TEST(ContainerTest, Printer) {
  EXPECT_THAT(
      TestPrintValue(std::vector{1, 2, 3}, Arbitrary<std::vector<int>>()),
      Each("{1, 2, 3}"));
  EXPECT_THAT(TestPrintValue(*Arbitrary<std::set<int>>().FromValue({1, 2, 3}),
                             Arbitrary<std::set<int>>()),
              Each("{1, 2, 3}"));
  EXPECT_THAT(TestPrintValue(
                  *Arbitrary<std::map<int, int>>().FromValue({{1, 2}, {2, 3}}),
                  Arbitrary<std::map<int, int>>()),
              Each("{{1, 2}, {2, 3}}"));

  // With custom inner
  auto inner = ElementOf({kGreen, kRed});
  using InnerCorpusT = corpus_type_t<decltype(inner)>;
  EXPECT_THAT(TestPrintValue(
                  std::list{InnerCorpusT{0}, InnerCorpusT{0}, InnerCorpusT{1}},
                  ContainerOf<std::vector<Color>>(inner)),
              ElementsAre("{Color{2}, Color{2}, Color{0}}",
                          "{static_cast<Color>(2), static_cast<Color>(2), "
                          "static_cast<Color>(0)}"));
}

TEST(DomainTest, Printer) {
  // Make sure we can print through the type erased Domain<T>
  auto color_domain = ElementOf({kBlue});
  auto print = [&](auto v, auto domain) {
    // We have to create the inner corpus_type of Domain here.
    return TestPrintValue(
        corpus_type_t<decltype(domain)>(std::in_place_type<decltype(v)>, v),
        domain);
  };
  EXPECT_THAT(print('a', Domain<char>(Arbitrary<char>())),
              ElementsAre("'a' (97)", "'a'"));
  EXPECT_THAT(print(corpus_type_t<decltype(color_domain)>{0},
                    Domain<Color>(color_domain)),
              ElementsAre("Color{1}", "static_cast<Color>(1)"));
}

TEST(VariantTest, Printer) {
  using V = std::variant<int, double, std::vector<std::string>>;
  V value;
  auto variant_domain = VariantOf<V>(
      Arbitrary<int>(), Arbitrary<double>(),
      ContainerOf<std::vector<std::string>>(Arbitrary<std::string>()));
  value = 1;
  EXPECT_THAT(TestPrintValue(value, variant_domain), Each("1"));
  value = 1.2;
  EXPECT_THAT(TestPrintValue(value, variant_domain), Each("1.2"));
  value = std::vector<std::string>{"variant", "print", "test"};
  EXPECT_THAT(TestPrintValue(value, variant_domain),
              Each("{\"variant\", \"print\", \"test\"}"));
}

TEST(OptionalTest, Printer) {
  auto optional_int_domain = OptionalOf(Arbitrary<int>());
  EXPECT_THAT(TestPrintValue({}, optional_int_domain), Each("std::nullopt"));
  EXPECT_THAT(
      TestPrintValue(corpus_type_t<Domain<int>>(std::in_place_type<int>, 1),
                     optional_int_domain),
      ElementsAre("(1)", "1"));

  auto optional_string_domain = OptionalOf(Arbitrary<std::string>());
  EXPECT_THAT(TestPrintValue({}, optional_string_domain), Each("std::nullopt"));
  EXPECT_THAT(TestPrintValue(corpus_type_t<Domain<std::string>>(
                                 std::in_place_type<std::string>, "ABC"),
                             optional_string_domain),
              ElementsAre("(\"ABC\")", "\"ABC\""));
}

TEST(SmartPointerTest, Printer) {
  EXPECT_THAT(TestPrintValue({}, Arbitrary<std::unique_ptr<int>>()),
              Each("nullptr"));
  EXPECT_THAT(
      TestPrintValue(corpus_type_t<Domain<int>>(std::in_place_type<int>, 7),
                     Arbitrary<std::unique_ptr<int>>()),
      ElementsAre("(7)", "std::make_unique<int>(7)"));
  EXPECT_THAT(
      TestPrintValue(corpus_type_t<Domain<std::string>>(
                         std::in_place_type<std::string>, "ABC"),
                     Arbitrary<std::shared_ptr<std::string>>()),
      ElementsAre(
          R"(("ABC"))",
          MatchesRegex(R"re(std::make_shared<std::.*string.*>\("ABC"\))re")));
}

TEST(OneOfTest, Printer) {
  auto domain = OneOf(ElementOf({17}), InRange(20, 22));
  using corpus_type = corpus_type_t<decltype(domain)>;

  EXPECT_THAT(TestPrintValue(corpus_type(std::in_place_index<0>), domain),
              Each("17"));
  EXPECT_THAT(TestPrintValue(corpus_type(std::in_place_index<1>, 21), domain),
              Each("21"));
}

std::string StringTimes(int n, char c) { return std::string(n, c); }

auto HexString() {
  return Map([](int number) { return absl::StrFormat("%#x", number); },
             InRange(0, 32));
}

TEST(MapTest, Printer) {
  auto domain = Map(StringTimes, InRange(2, 5), InRange('a', 'b'));
  std::tuple<int, char> corpus_value(3, 'b');

  EXPECT_THAT(TestPrintValue(corpus_value, domain),
              ElementsAre("\"bbb\"",
                          // Takes into account that the function name may
                          // contain ABI annotations after de-mangling.
                          MatchesRegex(R"re(StringTimes.*\(3, 'b'\))re")));

  // Test fallback on user value when map involves a lambda.
  EXPECT_THAT(TestPrintValue(std::tuple<int>(21), HexString()),
              Each("\"0x15\""));
}

}  // namespace
}  // namespace fuzztest::internal

auto DoubleValueOutsideNamespace(int n) { return 2 * n; }

namespace fuzztest::internal {
namespace {

TEST(MapTest, PrintsMapperOutsideNamespace) {
  auto domain = Map(DoubleValueOutsideNamespace, InRange(2, 5));
  std::tuple<int> corpus_value(3);

  EXPECT_THAT(
      TestPrintValue(corpus_value, domain),
      ElementsAre("6",
                  // Takes into account that the function name may
                  // contain ABI annotations after de-mangling.
                  MatchesRegex(R"re(DoubleValueOutsideNamespace.*\(3\))re")));
}

TEST(FlatMapTest, DelegatesToOutputDomainPrinter) {
  auto optional_sized_strings = [](int size) {
    return OptionalOf(String().WithSize(size));
  };
  auto input_domain = InRange(1, 3);
  auto flat_map_domain = FlatMap(optional_sized_strings, input_domain);

  corpus_type_t<decltype(flat_map_domain)> abc_corpus_val = {
      // String of size
      GenericDomainCorpusType(std::in_place_type<std::string>, "ABC"),
      // Size
      3};
  // Sanity checks that the components of `abc_corpus_val` are in the respective
  // domains.
  ASSERT_TRUE(
      input_domain.ValidateCorpusValue(std::get<1>(abc_corpus_val)).ok());
  ASSERT_TRUE(
      optional_sized_strings(input_domain.GetValue(std::get<1>(abc_corpus_val)))
          .ValidateCorpusValue(std::get<0>(abc_corpus_val))
          .ok());
  EXPECT_THAT(TestPrintValue(abc_corpus_val, flat_map_domain),
              ElementsAre("(\"ABC\")", "\"ABC\""));

  corpus_type_t<decltype(flat_map_domain)> nullopt_corpus_val = {
      // Corpus value of nullopt
      std::monostate{},
      // Size (here irrelevant)
      2};
  // Sanity checks that the components of `nullopt_corpus_val` are in the
  // respective domains.
  ASSERT_TRUE(
      input_domain.ValidateCorpusValue(std::get<1>(nullopt_corpus_val)).ok());
  ASSERT_TRUE(optional_sized_strings(
                  input_domain.GetValue(std::get<1>(nullopt_corpus_val)))
                  .ValidateCorpusValue(std::get<0>(nullopt_corpus_val))
                  .ok());
  EXPECT_THAT(TestPrintValue(nullopt_corpus_val, flat_map_domain),
              Each("std::nullopt"));
}

TEST(ConstructorOfTest, Printer) {
  EXPECT_THAT(
      TestPrintValue({3, 'b'}, ConstructorOf<std::string>(InRange(0, 5),
                                                          Arbitrary<char>())),
      ElementsAre("\"bbb\"", MatchesRegex(R"re(std::.*string.*\(3, 'b'\))re")));
  EXPECT_THAT(TestPrintValue(
                  {-1}, ConstructorOf<std::complex<float>>(Arbitrary<float>())),
              ElementsAre("(-1,0)", "std::complex<float>(-1.f)"));

  struct UserDefined {};
  EXPECT_THAT(TestPrintValue({}, ConstructorOf<UserDefined>()),
              ElementsAre("UserDefined{}", "UserDefined()"));
}

TEST(MonostateTest, Printer) {
  struct UserDefinedEmpty {};
  EXPECT_THAT(TestPrintValue(std::monostate{}), Each("{}"));
  EXPECT_THAT(TestPrintValue(std::tuple{}), Each("{}"));
  EXPECT_THAT(TestPrintValue(std::array<int, 0>{}), Each("{}"));
  EXPECT_THAT(TestPrintValue(UserDefinedEmpty{}), Each("UserDefinedEmpty{}"));
}

struct AggregateStructWithNoAbslStringify {
  int i = 1;
  std::pair<std::string, std::string> nested = {"Foo", "Bar"};
};

struct AggregateStructWithAbslStringify {
  int i = 1;
  std::pair<std::string, std::string> nested = {"Foo", "Bar"};

  template <typename Sink>
  friend void AbslStringify(Sink& sink,
                            const AggregateStructWithAbslStringify& s) {
    absl::Format(&sink, "value={%d, {%s, %s}}", s.i, s.nested.first,
                 s.nested.second);
  }
};

TEST(AutodetectAggregateTest, Printer) {
  // MonostateTest handles empty tuple and array.

  EXPECT_THAT(TestPrintValue(std::tuple{123}), Each("{123}"));
  EXPECT_THAT(TestPrintValue(std::pair{123, 456}), Each("{123, 456}"));
  EXPECT_THAT(TestPrintValue(std::array{123, 456}), Each("{123, 456}"));
  EXPECT_THAT(TestPrintValue(AggregateStructWithNoAbslStringify{}),
              Each(R"(AggregateStructWithNoAbslStringify{1, {"Foo", "Bar"}})"));
  EXPECT_THAT(
      TestPrintValue(AggregateStructWithAbslStringify{}),
      ElementsAre("value={1, {Foo, Bar}}",
                  R"(AggregateStructWithAbslStringify{1, {"Foo", "Bar"}})"));
}

TEST(DurationTest, Printer) {
  EXPECT_THAT(TestPrintValue(absl::InfiniteDuration()),
              ElementsAre("inf", "absl::InfiniteDuration()"));
  EXPECT_THAT(TestPrintValue(-absl::InfiniteDuration()),
              ElementsAre("-inf", "-absl::InfiniteDuration()"));
  EXPECT_THAT(TestPrintValue(absl::ZeroDuration()),
              ElementsAre("0", "absl::ZeroDuration()"));
  EXPECT_THAT(TestPrintValue(absl::Seconds(1)),
              ElementsAre("1s", "absl::Seconds(1)"));
  EXPECT_THAT(TestPrintValue(absl::Milliseconds(1500)),
              ElementsAre("1.5s",
                          "absl::Seconds(1) + "
                          "absl::Nanoseconds(500000000)"));
  EXPECT_THAT(TestPrintValue(absl::Nanoseconds(-0.25)),
              ElementsAre("-0.25ns",
                          "absl::Seconds(-1) + "
                          "(absl::Nanoseconds(1) / 4) * 3999999999"));
}

TEST(TimeTest, Printer) {
  EXPECT_THAT(TestPrintValue(absl::InfinitePast()),
              ElementsAre("infinite-past", "absl::InfinitePast()"));
  EXPECT_THAT(TestPrintValue(absl::InfiniteFuture()),
              ElementsAre("infinite-future", "absl::InfiniteFuture()"));
  EXPECT_THAT(TestPrintValue(absl::UnixEpoch()),
              ElementsAre("1970-01-01T00:00:00+00:00", "absl::UnixEpoch()"));
  EXPECT_THAT(TestPrintValue(absl::FromUnixSeconds(1577836800)),
              ElementsAre("2020-01-01T00:00:00+00:00",
                          "absl::UnixEpoch() + absl::Seconds(1577836800)"));
  EXPECT_THAT(TestPrintValue(absl::FromUnixSeconds(-1290000)),
              ElementsAre("1969-12-17T01:40:00+00:00",
                          "absl::UnixEpoch() + absl::Seconds(-1290000)"));
}

struct NonAggregateStructWithNoAbslStringify {
  NonAggregateStructWithNoAbslStringify() : i(1), nested("Foo", "Bar") {}
  int i;
  std::pair<std::string, std::string> nested;
};

struct NonAggregateStructWithAbslStringify {
  NonAggregateStructWithAbslStringify() : i(1), nested("Foo", "Bar") {}
  int i;
  std::pair<std::string, std::string> nested;

  template <typename Sink>
  friend void AbslStringify(Sink& sink,
                            const NonAggregateStructWithAbslStringify& s) {
    absl::Format(&sink, "value={%d, {%s, %s}}", s.i, s.nested.first,
                 s.nested.second);
  }
};

TEST(UnprintableTest, Printer) {
  EXPECT_THAT(TestPrintValue(NonAggregateStructWithNoAbslStringify{}),
              Each("<unprintable value>"));
  EXPECT_THAT(TestPrintValue(NonAggregateStructWithAbslStringify{}),
              ElementsAre("value={1, {Foo, Bar}}", "<unprintable value>"));
  EXPECT_THAT(
      TestPrintValue(std::vector<NonAggregateStructWithNoAbslStringify>{}),
      Each("<unprintable value>"));
  EXPECT_THAT(
      TestPrintValue(std::vector<NonAggregateStructWithAbslStringify>{}),
      Each("<unprintable value>"));
}

}  // namespace
}  // namespace fuzztest::internal
