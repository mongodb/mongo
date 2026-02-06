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

#define WEAK_SANCOV_DEF(return_type, name, ...)                           \
  extern "C" __attribute__((visibility("default"))) __attribute__((weak)) \
  return_type                                                             \
  name(__VA_ARGS__)

WEAK_SANCOV_DEF(void, __sanitizer_cov_trace_cmp, void) {}
WEAK_SANCOV_DEF(void, __sanitizer_cov_trace_cmp1, void) {}
WEAK_SANCOV_DEF(void, __sanitizer_cov_trace_cmp2, void) {}
WEAK_SANCOV_DEF(void, __sanitizer_cov_trace_cmp4, void) {}
WEAK_SANCOV_DEF(void, __sanitizer_cov_trace_cmp8, void) {}
WEAK_SANCOV_DEF(void, __sanitizer_cov_trace_const_cmp1, void) {}
WEAK_SANCOV_DEF(void, __sanitizer_cov_trace_const_cmp2, void) {}
WEAK_SANCOV_DEF(void, __sanitizer_cov_trace_const_cmp4, void) {}
WEAK_SANCOV_DEF(void, __sanitizer_cov_trace_const_cmp8, void) {}
WEAK_SANCOV_DEF(void, __sanitizer_cov_trace_switch, void) {}
WEAK_SANCOV_DEF(void, __sanitizer_cov_trace_div4, void) {}
WEAK_SANCOV_DEF(void, __sanitizer_cov_trace_div8, void) {}
WEAK_SANCOV_DEF(void, __sanitizer_cov_trace_gep, void) {}
WEAK_SANCOV_DEF(void, __sanitizer_cov_trace_pc_indir, void) {}
WEAK_SANCOV_DEF(void, __sanitizer_cov_8bit_counters_init, void) {}
WEAK_SANCOV_DEF(void, __sanitizer_cov_bool_flag_init, void) {}
WEAK_SANCOV_DEF(void, __sanitizer_cov_pcs_init, void) {}
WEAK_SANCOV_DEF(void, __sanitizer_cov_cfs_init, void) {}
WEAK_SANCOV_DEF(void, __sanitizer_cov_trace_pc_guard, void) {}
WEAK_SANCOV_DEF(void, __sanitizer_cov_trace_pc_guard_init, void) {}
WEAK_SANCOV_DEF(void, __sanitizer_cov_load1, void) {}
WEAK_SANCOV_DEF(void, __sanitizer_cov_load2, void) {}
WEAK_SANCOV_DEF(void, __sanitizer_cov_load4, void) {}
WEAK_SANCOV_DEF(void, __sanitizer_cov_load8, void) {}
WEAK_SANCOV_DEF(void, __sanitizer_cov_load16, void) {}
