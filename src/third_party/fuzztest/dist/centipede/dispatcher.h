// Copyright 2025 The Centipede Authors.
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

#ifndef THIRD_PARTY_CENTIPEDE_DISPATCHER_H_
#define THIRD_PARTY_CENTIPEDE_DISPATCHER_H_

// Dispatcher interface.
//
// This header needs to be C compatible.

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Inputs to perform mutations.
struct FuzzTestDispatcherInputForMutate {
  const void* input;
  size_t input_size;
  const void* metadata;
  size_t metadata_size;
};

// Callbacks to be provided by the fuzz testing framework to
// `FuzzTestDispatcherRun`.
struct FuzzTestDispatcherCallbacks {
  // Optional callback to return an ID for the current binary. If not
  // implemented, the controller will generate a default ID based on the binary
  // path.
  const char* (*get_binary_id)();
  // Callback to emit the list of available tests in the binary using
  // `FuzzTestDispatcherEmitTestName`.
  void (*list_tests)();
  // Callback to emit the seed inputs for a test using
  // `FuzzTestDispatcherEmitSeed`.
  void (*get_seeds)();
  // Optional callback to emit at most `num_mutants` from `inputs` with
  // `num_inputs` entries using `FuzzTestDispatcherEmitMutant`. `shrink` != 0
  // means to generate smaller mutants than the inputs used for mutation. If not
  // implemented, the controller will perform basic string-based mutations.
  //
  // TODO: xinhaoyuan - Reconsider mutation interface design instead of
  // following the existing Centipede/runner protocol.
  void (*mutate)(const struct FuzzTestDispatcherInputForMutate* inputs,
                 size_t num_inputs, size_t num_mutants, int shrink);
  // Callback to execute `input` with `size` bytes. The callback should emit
  // coverage feedback using `FuzzTestDispatcherEmitFeedback*` functions, and
  // any metadata for further mutation using
  // `FuzzTestDispatEmitExecutionMetadata`. In case the input caused a failure,
  // the callback should emit the failure using
  // `FuzzTestDispatcherEmitInputFailure`.
  void (*execute)(const void* input, size_t size);
};

// Functions provided by the FuzzTest engine.

// Returns 0 if the dispatcher mode is not enabled in the current process; 1 if
// the dispatcher mode is enabled; other values for unexpected errors.
int FuzzTestDispatcherIsEnabled();

// All functions below should be called only after `FuzzTestDispatcherIsEnabled`
// returns 1 in the current process.

// Returns the test name under operation as an unowned, static, and
// null-terminated string. Returns nullptr if the current process is not
// operating on a specific test.
const char* FuzzTestDispatcherGetTestName();

// Give control to the FuzzTest engine to invoke `callbacks`. Returns an exit
// code for the current process desired by the engine.
int FuzzTestDispatcherRun(const struct FuzzTestDispatcherCallbacks* callbacks);

// Emits a test name. Must be called from the `list_tests` callback. `name` must
// be a null-terminated string.
void FuzzTestDispatcherEmitTestName(const char* name);

// Emits a seed input. Must be called from the `get_seeds` callback. `data` must
// not be nullptr and `size > 0` must hold.
void FuzzTestDispatcherEmitSeed(const void* data, size_t size);

// Emits a mutant. Must be called from the `mutate` callback. `data` must not be
// nullptr and `size > 0` must hold.
void FuzzTestDispatcherEmitMutant(const void* data, size_t size);

// Emits coverage feedback for the current input as an array of 32-bit features.
//
// For each 32-bit feature, the bit [31] is ignored; the 4 bits [30-27]
// indicate the feature domain for engine prioritization. The remaining 27 bits
// [26-0] represent the actual 27-bit feature ID in the domain.
//
// Must be called from the `execute` callback. `features` must not be nullptr
// and `num_features > 0` must hold.
void FuzzTestDispatcherEmitFeedbackAs32BitFeatures(const uint32_t* features,
                                                   size_t num_features);
// Emits metadata of the current input as raw bytes. Must be called from
// the `execute` callback.
void FuzzTestDispatcherEmitExecutionMetadata(const void* metadata, size_t size);

// Functions for emitting various types of failures. After calling any of these
// functions, later calls of these functions would have no effect, and the
// current process should exit after necessary cleanup.

// Emits a failure caused by executing an input. Must be called within the
// `execute` callback. `description` should be a null-terminated string, or
// nullptr can be passed for an empty string; `signature` should be nullptr if
// and only if `signature_size == 0`.
void FuzzTestDispatcherEmitInputFailure(const char* description,
                                        const void* signature,
                                        size_t signature_size);

// Emits a failure that should be ignored (i.e. not affecting the fuzzing
// workflows). `description` should be a null-terminated string, or nullptr can
// be passed for an empty string.
void FuzzTestDispatcherEmitIgnoredFailure(const char* description);

// Emits a failure caused by the test setup. `description` should be a
// null-terminated string, or nullptr can be passed for an empty string.
void FuzzTestDispatcherEmitSetupFailure(const char* description);

// Emits a failure due to reasons to skip the entire test. `description` should
// be a null-terminated string, or nullptr can be passed for an empty string.
void FuzzTestDispatcherEmitSkippedTestFailure(const char* description);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif
