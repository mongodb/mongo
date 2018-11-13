// Copyright 2018 The Abseil Authors.
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

#include "absl/debugging/symbolize.h"

#if defined(ABSL_INTERNAL_HAVE_ELF_SYMBOLIZE)
#include "absl/debugging/symbolize_elf.inc"
#elif defined(_WIN32) && defined(_DEBUG)
// The Windows Symbolizer only works in debug mode. Note that _DEBUG
// is the macro that defines whether or not MS C-Runtime debug info is
// available. Note that the PDB files containing the debug info must
// also be available to the program at runtime for the symbolizer to
// work.
#include "absl/debugging/symbolize_win32.inc"
#else
#include "absl/debugging/symbolize_unimplemented.inc"
#endif
