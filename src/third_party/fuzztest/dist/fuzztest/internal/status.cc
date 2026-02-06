#include "./fuzztest/internal/status.h"

#include "absl/status/status.h"
#include "absl/strings/cord.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"

absl::Status SetMessage(const absl::Status& status, absl::string_view message) {
  absl::Status result(status.code(), message);
  status.ForEachPayload(
      [&](absl::string_view type_url, const absl::Cord& payload) {
        result.SetPayload(type_url, payload);
      });
  return result;
}

absl::Status Prefix(const absl::Status& status, absl::string_view prefix) {
  if (status.ok() || prefix.empty()) return status;
  return SetMessage(status, absl::StrCat(prefix, " >> ", status.message()));
}

absl::Status Postfix(const absl::Status& status, absl::string_view postfix) {
  if (status.ok() || postfix.empty()) return status;
  return SetMessage(status, absl::StrCat(status.message(), " >> ", postfix));
}
