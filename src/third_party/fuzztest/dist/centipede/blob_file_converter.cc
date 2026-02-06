// Copyright 2022 The Centipede Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <cstdint>
#include <cstdlib>
#include <filesystem>  // NOLINT
#include <string>

#include "absl/base/nullability.h"
#include "absl/flags/flag.h"
#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/strings/str_format.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "./centipede/config_init.h"
#include "./centipede/rusage_profiler.h"
#include "./common/blob_file.h"
#include "./common/defs.h"
#include "./common/logging.h"
#include "./common/remote_file.h"

ABSL_FLAG(std::string, in, "", "Input path");
ABSL_FLAG(std::string, out, "", "Output path");
ABSL_FLAG(std::string, out_format, "riegeli", "--out format (legacy|riegeli)");

namespace fuzztest::internal {
namespace {

// TODO(ussuri): Pare down excessive rusage profiling after breaking in.

class StatsLogger {
 public:
  StatsLogger(absl::Duration log_every, RUsageProfiler& rprof)
      : log_every_(log_every),
        next_log_at_(start_ + log_every),
        rprof_(rprof) {}

  void UpdateStats(ByteSpan blob) {
    ++num_blobs_;
    num_bytes_ += blob.size();
  }

  void Log() {
    RPROF_THIS_FUNCTION_BY_EXISTING_RPROF(rprof_);
    const auto secs = absl::ToDoubleSeconds(absl::Now() - start_);
    const std::string stats = absl::StrFormat(
        "blobs: %9lld | blobs/s: %5.0f | bytes: %12lld | bytes/s: %8.0f",
        num_blobs_, num_blobs_ / secs, num_bytes_, num_bytes_ / secs);
    if (ABSL_VLOG_IS_ON(3)) {
      const RUsageProfiler::Snapshot& snapshot = RPROF_SNAPSHOT(stats);
      LOG(INFO) << stats << " | " << snapshot.memory.ShortStr();
    } else {
      LOG(INFO) << stats;
    }
  }

  void MaybeLogIfTime() {
    const auto now = absl::Now();
    if (now >= next_log_at_) {
      Log();
      next_log_at_ += log_every_;
      if (next_log_at_ < now) next_log_at_ = now + log_every_;
    }
  }

 private:
  int64_t num_blobs_ = 0;
  int64_t num_bytes_ = 0;

  const absl::Time start_ = absl::Now();
  const absl::Duration log_every_;
  absl::Time next_log_at_;

  RUsageProfiler& rprof_;
};

void Convert(               //
    const std::string& in,  //
    const std::string& out, const std::string& out_format) {
  RPROF_THIS_FUNCTION_WITH_REPORT(/*enable=*/ABSL_VLOG_IS_ON(1));

  LOG(INFO) << "Converting:\n" << VV(in) << "\n" << VV(out) << VV(out_format);

  const bool out_is_riegeli = out_format == "riegeli";

  // Verify and prepare source and destination.

  CHECK(RemotePathExists(in)) << VV(in);
  CHECK_OK(RemoteMkdir(std::filesystem::path{out}.parent_path().c_str()));

  // Open blob file reader and writer.

  RPROF_START_TIMELAPSE(  //
      absl::Seconds(20), /*also_log=*/ABSL_VLOG_IS_ON(3), "Opening --in");
  const auto in_reader = DefaultBlobFileReaderFactory();
  CHECK_OK(in_reader->Open(in)) << VV(in);
  RPROF_STOP_TIMELAPSE();
  RPROF_SNAPSHOT_AND_LOG("Opened --in; opening --out");
  const auto out_writer = DefaultBlobFileWriterFactory(out_is_riegeli);
  CHECK_OK(out_writer->Open(out, "w")) << VV(out);
  RPROF_SNAPSHOT_AND_LOG("Opened --out");

  // Read and write blobs one-by-one.

  ByteSpan blob;
  absl::Status read_status = absl::OkStatus();
  StatsLogger stats_logger{
      absl::Seconds(ABSL_VLOG_IS_ON(1) ? 20 : 60),
      FUNCTION_LEVEL_RPROF_NAME,
  };
  while ((read_status = in_reader->Read(blob)).ok()) {
    CHECK_OK(out_writer->Write(blob));
    stats_logger.UpdateStats(blob);
    stats_logger.MaybeLogIfTime();
  }
  stats_logger.Log();
  CHECK(read_status.ok() || absl::IsOutOfRange(read_status)) << VV(read_status);
  CHECK_OK(out_writer->Close()) << VV(out);
}

}  // namespace
}  // namespace fuzztest::internal

int main(int argc, char** absl_nonnull argv) {
  (void)fuzztest::internal::InitRuntime(argc, argv);

  const std::string in = absl::GetFlag(FLAGS_in);
  QCHECK(!in.empty());
  const std::string out = absl::GetFlag(FLAGS_out);
  QCHECK(!out.empty());
  const std::string out_format = absl::GetFlag(FLAGS_out_format);
  QCHECK(out_format == "legacy" || out_format == "riegeli") << VV(out_format);

  fuzztest::internal::Convert(in, out, out_format);

  return EXIT_SUCCESS;
}
