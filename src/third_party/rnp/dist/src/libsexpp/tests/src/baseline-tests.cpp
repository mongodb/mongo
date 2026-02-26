/**
 *
 * Copyright 2021-2023 Ribose Inc. (https://www.ribose.com)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
 * the Software, and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 */

#include "sexp-tests.h"

using namespace sexp;
using ::testing::UnitTest;

namespace {
class BaselineTests : public testing::Test {
  protected:
    static const uint32_t base_sample_advanced = 0;
    static const uint32_t base_sample_base64 = 1;
    static const uint32_t base_sample_canonical = 2;
    static const uint32_t n_base_samples = base_sample_canonical + 1;

    static std::string base_samples[n_base_samples];

    BaselineTests()
    {
        base_samples[base_sample_advanced] = sexp_samples_folder + "/baseline/sexp-sample-a";
        base_samples[base_sample_base64] = sexp_samples_folder + "/baseline/sexp-sample-b";
        base_samples[base_sample_canonical] = sexp_samples_folder + "/baseline/sexp-sample-c";
    };
};

const uint32_t BaselineTests::n_base_samples;
const uint32_t BaselineTests::base_sample_advanced;
const uint32_t BaselineTests::base_sample_base64;
const uint32_t BaselineTests::base_sample_canonical;
std::string    BaselineTests::base_samples[n_base_samples];

TEST_F(BaselineTests, Scan2Canonical)
{
    for (uint32_t i = 0; i < n_base_samples; i++) {
        std::ifstream ifs(base_samples[i], std::ifstream::binary);
        bool          r = ifs.fail();
        EXPECT_FALSE(r);

        if (!ifs.fail()) {
            sexp_input_stream_t is(&ifs);
            const auto          obj = is.set_byte_size(8)->get_char()->scan_object();

            std::ostringstream   oss(std::ios_base::binary);
            sexp_output_stream_t os(&oss);
            os.print_canonical(obj);

            std::istringstream iss(oss.str(), std::ios_base::binary);
            EXPECT_TRUE(compare_binary_files(base_samples[base_sample_canonical], iss));
        }
    }
}

TEST_F(BaselineTests, Scan2Base64)
{
    for (uint32_t i = 0; i < n_base_samples; i++) {
        std::ifstream ifs(base_samples[i], std::ifstream::binary);
        EXPECT_FALSE(ifs.fail());

        if (!ifs.fail()) {
            sexp_input_stream_t is(&ifs);
            const auto          obj = is.set_byte_size(8)->get_char()->scan_object();

            std::ostringstream   oss(std::ios_base::binary);
            sexp_output_stream_t os(&oss);

            os.set_max_column(0)->print_base64(obj);
            oss << std::endl;

            std::istringstream iss(oss.str(), std::ios_base::binary);
            EXPECT_TRUE(compare_text_files(base_samples[base_sample_base64], iss));
        }
    }
}

TEST_F(BaselineTests, Scan2Advanced)
{
    for (uint32_t i = 0; i < n_base_samples; i++) {
        std::ifstream ifs(base_samples[i], std::ifstream::binary);
        EXPECT_FALSE(ifs.fail());

        if (!ifs.fail()) {
            sexp_input_stream_t is(&ifs);
            const auto          obj = is.set_byte_size(8)->get_char()->scan_object();

            std::ostringstream   oss(std::ios_base::binary);
            sexp_output_stream_t os(&oss);
            os.print_advanced(obj);

            std::istringstream iss(oss.str(), std::ios_base::binary);
            EXPECT_TRUE(compare_text_files(base_samples[base_sample_advanced], iss));
        }
    }
}
} // namespace
