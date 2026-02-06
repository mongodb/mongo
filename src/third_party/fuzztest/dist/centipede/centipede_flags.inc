// Copyright 2023 The Centipede Authors.
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
//
// Definitions of the Centipede flags to be expanded in different contexts.
// Each flag is defined as:
//
//   CENTIPEDE_FLAG(type, name, default_value, description)
//
// default_value must be a compile-time constant.
// description must be a string literal.

#ifndef CENTIPEDE_FLAG
#error This file must be used only in Centipede with CENTIPEDE_FLAG defined.
#endif

// TODO(kcc): document usage of standalone binaries and how to use @@ wildcard.
// If the "binary" contains @@, it means the binary can only accept inputs
// from the command line, and only one input per process.
// @@ will be replaced with a path to file with the input.
// @@ is chosen to follow the AFL command line syntax.
// TODO(kcc): rename --binary to --command (same for --extra_binaries),
// while remaining backward compatible.
CENTIPEDE_FLAG(std::string, binary, "", "The target binary.")
CENTIPEDE_FLAG(
    std::string, coverage_binary, "",
    "The actual binary from which coverage is collected - if different "
    "from --binary.")
CENTIPEDE_FLAG(
    std::string, binary_hash, "",
    "If not-empty, this hash string is used instead of the hash of the "
    "contents of coverage_binary. Use this flag when the coverage_binary "
    "is not available nor needed, e.g. when using --distill.")
CENTIPEDE_FLAG(
    std::string, clang_coverage_binary, "",
    "A clang source-based code coverage binary used to produce "
    "human-readable reports. Do not add this binary to extra_binaries. "
    "You must have llvm-cov and llvm-profdata in your path to generate "
    "the reports. --workdir in turn must be local in order for this "
    "functionality to work. See "
    "https://clang.llvm.org/docs/SourceBasedCodeCoverage.html")
CENTIPEDE_FLAG(std::string, test_name, "",
               "The name of test to pass to the binary to operate on.")
CENTIPEDE_FLAG(
    std::vector<std::string>, extra_binaries, {},
    "A comma-separated list of extra target binaries. These binaries are "
    "fed the same inputs as the main binary, but the coverage feedback "
    "from them is not collected. Use this e.g. to run the target under "
    "sanitizers.")
CENTIPEDE_FLAG(std::string, workdir, "", "The working directory.")
CENTIPEDE_FLAG(
    std::string, merge_from, "",
    "Another working directory to merge the corpus from. Inputs from "
    "--merge_from will be added to --workdir if the add new features.")
CENTIPEDE_FLAG(size_t, num_runs, std::numeric_limits<size_t>::max(),
               "Number of inputs to run per shard (see --total_shards).")
CENTIPEDE_FLAG(size_t, total_shards, 1, "Number of shards.")
CENTIPEDE_FLAG(size_t, my_shard_index, 0,
               "Index of the first shard, [0, --total_shards - --num_threads].")
CENTIPEDE_FLAG(
    size_t, num_threads, 1,
    "Number of threads to execute in one process. i-th thread, where i "
    "is in [0, --num_threads), will work on shard "
    "(--first_shard_index + i).")
CENTIPEDE_FLAG(size_t, j, 0,
               "If not 0, --j=N is a shorthand for "
               "--num_threads=N --total_shards=N --first_shard_index=0. "
               "Overrides values of these flags if they are also used.")
CENTIPEDE_FLAG(size_t, max_len, 4000,
               "Max length of mutants. Passed to mutator as a hint.")
CENTIPEDE_FLAG(
    size_t, batch_size, 1000,
    "The number of inputs given to the target at one time. Batches of "
    "more than 1 input are used to amortize the process start-up cost.")
CENTIPEDE_FLAG(size_t, mutate_batch_size, 2,
               "Mutate this many inputs to produce batch_size mutants")
CENTIPEDE_FLAG(
    bool, use_legacy_default_mutator, false,
    "When set, use the legacy ByteArrayMutator as the default mutator. "
    "Otherwise, the FuzzTest domain based mutator will be used.")
CENTIPEDE_FLAG(
    size_t, load_other_shard_frequency, 10,
    "Load a random other shard after processing this many batches. Use 0 "
    "to disable loading other shards.  For now, choose the value of this "
    "flag so that shard loads happen at most once in a few minutes. In "
    "future we may be able to find the suitable value automatically.")
// TODO(b/262798184): Remove once the bug is fixed.
CENTIPEDE_FLAG(
    bool, serialize_shard_loads, false,
    "When this flag is on, shard loading is serialized. "
    " Useful to avoid excessive RAM consumption when loading more"
    " that one shard at a time. Currently, loading a single large shard"
    " may create too many temporary heap allocations. "
    " This means, if we load many large shards concurrently,"
    " we may run out or RAM.")
CENTIPEDE_FLAG(
    size_t, seed, 0,
    "A seed for the random number generator. If 0, some other random "
    "number is used as seed.")
CENTIPEDE_FLAG(
    size_t, prune_frequency, 100,
    "Prune the corpus every time after this many inputs were added. If "
    "zero, pruning is disabled. Pruning removes redundant inputs from "
    "the corpus, e.g. inputs that have only \"frequent\", i.e. "
    "uninteresting features. When the corpus gets larger than "
    "--max_corpus_size, some random elements may also be removed.")
CENTIPEDE_FLAG(
    size_t, address_space_limit_mb,
#ifdef __APPLE__
    // Address space limit is ignored on MacOS.
    // Reference:
    // https://bugs.chromium.org/p/chromium/issues/detail?id=853873#c2
    0
#else
    8192
#endif
    ,
    "If not zero, instructs the target to set setrlimit(RLIMIT_AS) to "
    "this number of megabytes. Some targets (e.g. if built with ASAN, "
    "which can't run with RLIMIT_AS) may choose to ignore this flag. See "
    "also --rss_limit_mb.")
CENTIPEDE_FLAG(
    size_t, rss_limit_mb, 4096,
    "If not zero, instructs the target to fail if RSS goes over this "
    "number of megabytes and report an OOM. See also "
    "--address_space_limit_mb. These two flags have somewhat different "
    "meaning. --address_space_limit_mb does not allow the process to "
    "grow the used address space beyond the limit. --rss_limit_mb runs a "
    "background thread that monitors max RSS and also checks max RSS "
    "after executing every input, so it may detect OOM late. However "
    "--rss_limit_mb allows Centipede to *report* an OOM condition in "
    "most cases, while --address_space_limit_mb will cause a crash that "
    "may be hard to attribute to OOM.")
CENTIPEDE_FLAG(
    size_t, stack_limit_kb, 0,
    "If not zero, instructs the target to fail if stack usage goes over "
    "this number of KiB.")
CENTIPEDE_FLAG(
    size_t, timeout_per_input, 60,
    "If not zero, the timeout in seconds for a single input. If an input "
    "runs longer than this, the runner process will abort. Support may "
    "vary depending on the runner.")
CENTIPEDE_FLAG(
    size_t, timeout_per_batch, 0,
    "If not zero, the collective timeout budget in seconds for a single "
    "batch of inputs. Each input in a batch still has up to "
    "--timeout_per_input seconds to finish, but the entire batch must "
    "finish within --timeout_per_batch seconds. The default is computed "
    "as a function of --timeout_per_input * --batch_size. Support may "
    "vary depending on the runner.")
CENTIPEDE_FLAG(size_t, ignore_timeout_reports, false,
               "If set, will ignore reporting timeouts as errors.")
CENTIPEDE_FLAG(
    absl::Time, stop_at, absl::InfiniteFuture(),
    "Stop fuzzing in all shards (--total_shards) at approximately this "
    "time in ISO-8601/RFC-3339 format, e.g. 2023-04-06T23:35:02Z. "
    "If a given shard is still running at that time, it will gracefully "
    "wind down by letting the current batch of inputs to finish and then "
    "exiting. A special value 'infinite-future' (the default) is "
    "supported. Tip: `date` is useful for conversion of mostly free "
    "format human readable date/time strings, e.g. "
    "--stop_at=$(date --date='next Monday 6pm' --utc --iso-8601=seconds) "
    ". Also see --stop_after. These two flags are mutually exclusive.")
CENTIPEDE_FLAG(
    bool, fork_server, true,
    "If true (default) tries to execute the target(s) via the fork "
    "server, if supported by the target(s). Prepend the binary path with "
    "'%f' to disable the fork server. --fork_server applies to binaries "
    "passed via these flags: --binary, --extra_binaries, "
    "--input_filter.")
CENTIPEDE_FLAG(
    bool, full_sync, false,
    "Perform a full corpus sync on startup. If true, feature sets and "
    "corpora are read from all shards before fuzzing. This way fuzzing "
    "starts with a full knowledge of the current state and will avoid "
    "adding duplicating inputs. This however is very expensive when the "
    "number of shards is very large.")
CENTIPEDE_FLAG(
    bool, use_corpus_weights, true,
    "If true, use weighted distribution when choosing the corpus element "
    "to mutate. This flag is mostly for Centipede developers.")
CENTIPEDE_FLAG(
    bool, use_coverage_frontier, false,
    "If true, use coverage frontier when choosing the corpus element to "
    "mutate. This flag is mostly for Centipede developers.")
CENTIPEDE_FLAG(
    size_t, max_corpus_size, 100000,
    "Indicates the number of inputs in the in-memory corpus after which"
    "more aggressive pruning will be applied.")
CENTIPEDE_FLAG(
    size_t, crossover_level, 50,
    "Defines how much crossover is used during mutations. 0 means no "
    "crossover, 100 means the most aggressive crossover. See "
    "https://en.wikipedia.org/wiki/Crossover_(genetic_algorithm).")
CENTIPEDE_FLAG(bool, use_pc_features, true,
               "When available from instrumentation, use features derived from "
               "PCs.")
CENTIPEDE_FLAG(
    size_t, path_level, 0,  // Not ready for wide usage.
    "When available from instrumentation, use features derived from "
    "bounded execution paths. Be careful, may cause exponential feature "
    "explosion. 0 means no path features. Values between 1 and 100 "
    "define how aggressively to use the paths.")
CENTIPEDE_FLAG(bool, use_cmp_features, true,
               "When available from instrumentation, use features derived from "
               "instrumentation of CMP instructions.")
CENTIPEDE_FLAG(
    size_t, callstack_level, 0,
    "When available from instrumentation, use features derived from "
    "observing the function call stacks. 0 means no callstack features."
    "Values between 1 and 100 define how aggressively to use the "
    "callstacks. Level N roughly corresponds to N call frames.")
CENTIPEDE_FLAG(bool, use_auto_dictionary, true,
               "If true, use automatically-generated dictionary derived from "
               "intercepting comparison instructions, memcmp, and similar.")
CENTIPEDE_FLAG(bool, use_dataflow_features, true,
               "When available from instrumentation, use features derived from "
               "data flows.")
CENTIPEDE_FLAG(
    bool, use_counter_features, false,
    "When available from instrumentation, use features derived from "
    "counting the number of occurrences of a given PC. When enabled, "
    "supersedes --use_pc_features.")
CENTIPEDE_FLAG(bool, use_pcpair_features, false,
               "If true, PC pairs are used as additional synthetic features. "
               "Experimental, use with care - it may explode the corpus.")
CENTIPEDE_FLAG(
    uint64_t, user_feature_domain_mask, ~0UL,
    "A bitmask indicating which user feature domains should be enabled. "
    "A value of zero will disable all user features.")
CENTIPEDE_FLAG(
    size_t, feature_frequency_threshold, 100,
    "Internal flag. When a given feature is present in the corpus this "
    "many times Centipede will stop recording it for future corpus "
    "elements. Larger values will use more RAM but may improve corpus "
    "weights. Valid values are 2 - 255.")
CENTIPEDE_FLAG(bool, require_pc_table, true,
               "If true, Centipede will exit if the --pc_table is not found.")
CENTIPEDE_FLAG(bool, require_seeds, false,
               "If true, Centipede will exit if no seed inputs are found.")
CENTIPEDE_FLAG(
    int, telemetry_frequency, 0,
    "Dumping frequency for intermediate telemetry files, i.e. coverage "
    "report (workdir/coverage-report-BINARY.*.txt), corpus stats "
    "(workdir/corpus-stats-*.json), etc. Positive value N means dump "
    "every N batches. Negative N means start dumping after 2^N processed "
    "batches with exponential 2x back-off (e.g. for "
    "--telemetry_frequency=-5, dump on batches 32, 64, 128,...). Zero "
    "means no telemetry. Note that the before-fuzzing and after-fuzzing "
    "telemetry are always dumped.")
CENTIPEDE_FLAG(bool, print_runner_log, false,
               "If true, runner logs are printed after every batch. Note that "
               "crash logs are always printed regardless of this flag's value.")
// TODO(kcc): --distill and several others had better be dedicated binaries.
CENTIPEDE_FLAG(
    bool, distill, false,
    "Distill (minimize) the --total_shards input shards from --workdir "
    "into --num_threads output shards. The input shards are randomly and "
    "evenly divided between --num_threads concurrent distillation "
    "threads to speed up processing. The threads share and update the "
    "global coverage info as they go, so the output shards will never "
    "have identical input/feature pairs (some intputs can still be "
    "identical if a non-deterministic target produced different features "
    "for identical inputs in the corpus). The features.* files are "
    "looked up in a --workdir subdirectory that corresponds to "
    "--coverage_binary and --binary_hash, if --binary_hash is provided; "
    "if it is not provided, the actual hash of the --coverage_binary "
    "file on disk is computed and used. Therefore, with an explicit "
    "--binary_hash, --coverage_binary can be just the basename of the "
    "actual target binary; without it, it must be the full path. "
    "Each distillation thread writes a distilled corpus shard to "
    "to <--workdir>/distilled-<--coverage_binary basename>.<index>.")
CENTIPEDE_FLAG(
    size_t, log_features_shards, 0,
    "The first --log_features_shards shards will log newly observed "
    "features as symbols. In most cases you don't need this to be >= 2.")
CENTIPEDE_FLAG(
    std::string, knobs_file, "",
    "If not empty, knobs will be read from this (possibly remote) file."
    " The feature is experimental, not yet fully functional.")
CENTIPEDE_FLAG(
    std::string, corpus_to_files, "",
    "Save the remote corpus from working to the given directory, one "
    "file per corpus.")
CENTIPEDE_FLAG(
    std::string, crashes_to_files, "",
    "When set to a directory path, save the crashing reproducers and "
    "metadata from the workdir to the given path: Each crash with `ID`"
    "will be saved with file `ID.data` for the reproducer, `ID.desc` the "
    "description, `ID.sig` the signature. If multiple crashes with the same ID "
    "exist, only one crash will be saved.")
CENTIPEDE_FLAG(
    std::string, corpus_from_files, "",
    "Export a corpus from a local directory with one file per input into "
    "the sharded remote corpus in workdir. Not recursive.")
CENTIPEDE_FLAG(
    std::vector<std::string>, corpus_dir, {},
    "Comma-separated list of paths to local corpus dirs, with one file "
    "per input. At startup, the files are exported into the corpus in "
    "--workdir. While fuzzing, the new corpus elements are written to "
    "the first dir if it is not empty. This makes it more convenient to "
    "interop with libFuzzer corpora.")
CENTIPEDE_FLAG(
    std::string, symbolizer_path, "llvm-symbolizer",
    "Path to the symbolizer tool. By default, we use llvm-symbolizer "
    "and assume it is in PATH.")
CENTIPEDE_FLAG(
    std::string, objdump_path, "objdump",
    "Path to the objdump tool. By default, we use the system objdump "
    "and assume it is in PATH.")
CENTIPEDE_FLAG(std::string, runner_dl_path_suffix, "",
               "If non-empty, this flag is passed to the Centipede runner. "
               "It tells the runner that this dynamic library is instrumented "
               "while the main binary is not. "
               "The value could be the full path, like '/path/to/my.so' "
               "or a suffix, like '/my.so' or 'my.so'."
               "This flag is experimental and may be removed in future")
CENTIPEDE_FLAG(
    std::string, input_filter, "",
    "Path to a tool that filters bad inputs. The tool is invoked as "
    "`input_filter INPUT_FILE` and should return 0 if the input is good "
    "and non-0 otherwise. Ignored if empty. The --input_filter is "
    "invoked only for inputs that are considered for addition to the "
    "corpus.")
CENTIPEDE_FLAG(
    std::vector<std::string>, dictionary, {},
    "A comma-separated list of paths to dictionary files. The dictionary "
    "file is either in AFL/libFuzzer plain text format or in the binary "
    "Centipede corpus file format. The flag is interpreted by "
    "CentipedeCallbacks so its meaning may be different in custom "
    "implementations of CentipedeCallbacks.")
CENTIPEDE_FLAG(
    std::string, function_filter, "",
    "A comma-separated list of functions that fuzzing needs to focus on. "
    "If this list is non-empty, the fuzzer will mutate only those inputs "
    "that trigger code in one of these functions.")
CENTIPEDE_FLAG(
    std::string, for_each_blob, "",
    "If non-empty, extracts individual blobs from the files given as "
    "arguments, copies each blob to a temporary file, and applies this "
    "command to that temporary file. %P is replaced with the temporary "
    "file's path and %H is replaced with the blob's hash. Example:\n"
    "$ centipede --for_each_blob='ls -l  %P && echo %H' corpus.000000")
CENTIPEDE_FLAG(
    std::string, experiment, "",
    "A colon-separated list of values, each of which is a flag followed "
    "by = and a comma-separated list of values. Example: "
    "'foo=1,2,3:bar=10,20'. When non-empty, this flag is used to run an "
    "A/B[/C/D...] experiment: different threads will set different "
    "values of 'foo' and 'bar' and will run independent fuzzing "
    "sessions. If more than one flag is given, all flag combinations are "
    "tested. In example above: '--foo=1 --bar=10' ... "
    "'--foo=3 --bar=20'. The number of threads should be multiple of the "
    "number of flag combinations.")
CENTIPEDE_FLAG(
    bool, analyze, false,
    "If set, Centipede will read the corpora from the work dirs provided"
    " as argv. If two corpora are provided, then analyze differences"
    " between those corpora. If one corpus is provided, then save the"
    " coverage report to a file within workdir with prefix"
    " 'coverage-report-'.")
CENTIPEDE_FLAG(bool, exit_on_crash, false,
               "If true, Centipede will exit on the first crash of the target.")
CENTIPEDE_FLAG(size_t, max_num_crash_reports, 5,
               "report this many crashes per shard.")
CENTIPEDE_FLAG(
    std::string, minimize_crash_file_path, "",
    "If non-empty, a path to an input file that triggers a crash."
    " Centipede will run the minimization loop and store smaller crashing"
    " inputs in workdir/crashes.NNNNNN/, where NNNNNN is "
    "--first_shard_index padded on the left with zeros. "
    " --num_runs and --num_threads apply. "
    " Assumes local workdir.")
CENTIPEDE_FLAG(
    bool, batch_triage_suspect_only, false,
    "If set, triage the crash on only the suspected input in a crashing "
    "batch. Otherwise, triage on all the executed inputs")
CENTIPEDE_FLAG(
    size_t, shmem_size_mb, 1024,
    "Size of the shared memory regions used to communicate between the "
    "ending and the runner.")
CENTIPEDE_FLAG(
    bool, use_posix_shmem,
#ifdef __APPLE__
    true
#else
    false
#endif
    ,
    "[INTERNAL] When true, uses shm_open/shm_unlink instead of "
    "memfd_create to allocate shared memory. You may want this if your "
    "target doesn't have access to /proc/<arbitrary_pid> subdirs or the "
    "memfd_create syscall is not supported.")
CENTIPEDE_FLAG(
    bool, dry_run, false,
    "Initializes as much of Centipede as possible without actually "
    "running any fuzzing. Useful to validate the rest of the command "
    "line, verify existence of all the input directories and files, "
    "etc. Also useful in combination with --save_config or "
    "--update_config to stop execution immediately after writing the "
    "(updated) config file.")
CENTIPEDE_FLAG(bool, save_binary_info, false,
               "Save the BinaryInfo from the fuzzing run within the working "
               "directory.")
CENTIPEDE_FLAG(
    bool, populate_binary_info, true,
    "Get binary info from a coverage instrumented binary. This should "
    "only be turned off when coverage is not based on instrumenting some "
    "binary.")
CENTIPEDE_FLAG(
    bool, riegeli,
#ifdef CENTIPEDE_DISABLE_RIEGELI
    false
#else
    true
#endif
    ,
    "Use Riegeli file format (instead of the legacy bespoke encoding) "
    "for storage")
CENTIPEDE_FLAG(bool, first_corpus_dir_output_only, false,
               "If set, treat the first entry of `corpus_dir` as output-only. "
               "For FuzzTest framework only, do not use from end-users.")
CENTIPEDE_FLAG(bool, load_shards_only, false,
               "If set, load/merge shards without fuzzing new inputs. For "
               "FuzzTest framework only, do not use from end-users.")
CENTIPEDE_FLAG(
    bool, fuzztest_single_test_mode, false,
    "If set, operate on the corpus database for a single test specified by "
    "FuzzTest instead of all the tests. For FuzzTest framework only, do not "
    "use from end-users.")
CENTIPEDE_FLAG(
    std::string, fuzztest_configuration, "",
    "If set, deserializes the FuzzTest configuration from the value as a "
    "base64url string instead of querying the configuration via runner "
    "callbacks. For FuzzTest framework only, do not use from end-users.")
CENTIPEDE_FLAG(
    bool, list_crash_ids, false,
    "If set, lists the crash IDs of a single test of the binary to the "
    "`crash_ids_file` with each crash ID in a single line. If there is no "
    "crash for the test, the empty content will be written to the file. For "
    "FuzzTest framework only, do not use from end-users.")
CENTIPEDE_FLAG(std::string, list_crash_ids_file, "",
               "The path to list the crash IDs for `list_crash_ids`. For "
               "FuzzTest framework only, do not use from end-users.")
CENTIPEDE_FLAG(std::string, crash_id, "",
               "The crash ID used for `replay_crash` or `export_crash`. For "
               "FuzzTest framework only, do not use from end-users.")
CENTIPEDE_FLAG(bool, replay_crash, false,
               "If set, replay `crash_id` in the corpus database. For FuzzTest "
               "framework only, do not use from end-users.")
CENTIPEDE_FLAG(
    bool, export_crash, false,
    "If set, export the input contents of `crash_id` from the corpus database. "
    "For FuzzTest framework only, do not use from end-users.")
CENTIPEDE_FLAG(
    std::string, export_crash_file, "",
    "The path to export the input contents of `crash_id` for `export_crash`. "
    "For FuzzTest framework only, do not use from end-users.")
