#!/usr/bin/env bash

# Whenever Bazel is invoked, it first calls this script setting "BAZEL_REAL" to the path of the real Bazel binary.
# Use this file as a wrapper for any logic that should run before bazel itself is executed.

# WARNING : If you run //:compiledb target, you can not print to stdout in this file as it will fail with
# "Bazel aquery failed." because it is reading this files stdout as aquery output

REPO_ROOT=$(dirname $(dirname $(realpath "$0")))
bazel_real="$BAZEL_REAL"



# If disk space becomes an issue, this block can be used to clean up configs
# when disk space is getting low. It is commented out because "bazel info output_path"
# takes .5 seconds, and no need to add that extra time unless disk space becomes a large
# problem

#if [ $# -eq 2 ] || [ $# -eq 3 ]; then
#    if [ "$1" = "build" ]; then
#        output_path=$($bazel_real info output_path)
#        remaining_kb=$(df --output=avail $output_path | tail -n 1)
#        remaining_gb=$(($remaining_kb / 1024 / 1024))
#        fastbuild_dir=${output_path}/aarch64-fastbuild
#        dbg_dir=${output_path}/aarch64-dbg
#        opt_dir=${output_path}/aarch64-opt
#        if [ "$remaining_gb" -lt 40 ]; then
#          echo "Disk space is getting low (under 40GB) - cleaning up other config outputs"
#          if [ $# -eq 2 ]; then
#            rm -rf "$dbg_dir" &>/dev/null
#            rm -rf "$opt_dir" &>/dev/null
#          elif [[ "$2" == "--config=fastbuild"* ]]; then
#            rm -rf "$dbg_dir" &>/dev/null
#            rm -rf "$opt_dir" &>/dev/null
#          elif [[ "$2" == "--config=dbg"* ]]; then
#            rm -rf "$fastbuild_dir" &>/dev/null
#            rm -rf "$opt_dir" &>/dev/null
#          elif [[ "$2" == "--config=opt"* ]]; then
#            rm -rf "$fastbuild_dir" &>/dev/null
#            rm -rf "$dbg_dir" &>/dev/null
#          fi
#        fi
#    fi
#fi

if [[ -z "${BAZELISK_SKIP_WRAPPER}" ]]; then
    echo "You're not using Bazelisk, which is recommended for a consistent build environment." >&2
    echo "Your version of Bazel may be mismatched with the version intended to be used to build MongoDB." >&2
    echo "Please run the following command to install Bazelisk and make sure to add ~/.local/bin to your PATH:" >&2
    echo "" >&2
    echo "python buildscripts/install_bazel.py" >&2
    exit 0
fi

echo "common --//bazel/config:running_through_bazelisk" > $REPO_ROOT/.bazelrc.bazelisk

# Write a compressed execution log to a file for EngFlow to pick up for more detailed analysis.
echo "common --execution_log_compact_file=$REPO_ROOT/.tmp/bazel_execution_log.binpb.zst" > $REPO_ROOT/.bazelrc.exec_log_file

if [[ $MONGO_BAZEL_WRAPPER_DEBUG == 1 ]]; then
    wrapper_start_time="$(date -u +%s.%N)"
fi
 
# detect os and arch so we can try to find python later
if [[ "$OSTYPE" == "linux"* ]]; then
  os="linux"
elif [[ "$OSTYPE" == "darwin"* ]]; then
  os="macos"
else
  os="unknown"
fi

ARCH=$(uname -m)
if [[ "$ARCH" == "arm64" || "$ARCH" == "aarch64" ]]; then
  ARCH="arm64"
elif [[ "$ARCH" == "ppc64le" || "$ARCH" == "ppc64" || "$ARCH" == "ppc" || "$ARCH" == "ppcle" ]]; then
  ARCH="ppc64le"
elif [[ "$ARCH" == "s390x" || "$ARCH" == "s390" ]]; then
  ARCH="s390x"
else
  ARCH="x86_64"
fi

# setup usual locations to look for certs for python to use
declare -a cert_locs=()
cert_locs+=("/etc/ssl/certs/ca-certificates.crt")                # Debian/Ubuntu/Gentoo etc.
cert_locs+=("/etc/pki/tls/certs/ca-bundle.crt")                  # Fedora/RHEL 6
cert_locs+=("/etc/ssl/ca-bundle.pem")                            # OpenSUSE
cert_locs+=("/etc/pki/tls/cacert.pem")                           # OpenELEC
cert_locs+=("/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem") # CentOS/RHEL 7
cert_locs+=("/etc/ssl/cert.pem")                                 # Alpine Linux

for cert in ${cert_locs[@]}; do
  if [ -f $cert ]; then
    export SSL_CERT_DIR=$(dirname $cert)
    export SSL_CERT_FILE=$cert
    break
  fi
done

# skip python if the bazel command type is in a known list of commands to skip
skip_python=0
bazel_commands=()
while IFS= read -r line; do
  bazel_commands+=("$line")
done < $REPO_ROOT/bazel/wrapper_hook/bazel_commands.commands

current_bazel_command=""
for arg do
  for command in "${bazel_commands[@]}"
  do
    if [ "$command" == "$arg" ] ; then
      current_bazel_command="$arg"
      break
    fi
  done
  if [ ! -z $current_bazel_command ]; then
    break
  fi
done

if [ -z $current_bazel_command ]; then
  skip_python=1
fi
SLOW_PATH=0  
if [[ "$skip_python" == "0" ]]; then  
  # We'll do wrapper hook / python installation  
  SLOW_PATH=1  
fi


if [ "$skip_python" == "0" ]; then
  # known list of commands to skip
  skip_commands=("clean" "version" "shutdown" "info")

  for command in "${skip_commands[@]}"
  do
    if [ "$current_bazel_command" == "$command" ] ; then
      skip_python=1
      break
    fi
  done
fi

if [ "$skip_python" == "1" ]; then  
  # Fast path: no wrapper output, run Bazel directly to terminal  
  exec "$bazel_real" "$@"  
fi  


if [[ "$SLOW_PATH" == "1" ]] && [[ -z "${MONGO_WRAPPER_OUTPUT_ALL}" ]]; then
    ORIGINAL_ARGS=("$@")      
  
    # Save original terminal output FDs
    exec 3>&1 4>&2      
    export MONGO_WRAPPER_STDOUT_FD=3      
    export MONGO_WRAPPER_STDERR_FD=4        
  
    LOG_DIR=${MONGO_BAZEL_LOG_DIR:-"$REPO_ROOT/.bazel_logs"}      
    mkdir -p "$LOG_DIR"      
    LOGFILE="${LOG_DIR}/bazel_wrapper_$(date +%Y%m%d_%H%M%S)_$$.log"      
  
    # Redirect stdout/stderr to logfile  
    exec >"$LOGFILE" 2>&1      
  
    WRAPPER_START_EPOCH=$(date +%s)      
  
    GREEN='\033[0;32m'      
    RED='\033[1;31m'      
    NO_COLOR='\033[0m'      
  
    echo -e "${GREEN}INFO:${NO_COLOR} running wrapper hook..." >&4      

    cleanup_logfile() {
        if [[ -n "$LOGFILE" && -f "$LOGFILE" ]]; then
            rm -f "$LOGFILE"
        fi
    }

    print_summary() {
        local exit_code=$?
        local end_epoch duration

        end_epoch=$(date +%s)
        duration=$(( end_epoch - WRAPPER_START_EPOCH ))

        # Only noisy on failure; quiet on success
        if [[ $exit_code -ne 0 ]]; then
            echo -e "${RED}ERROR:${NO_COLOR} wrapper hook failed (exit ${exit_code}, ${duration}s)." >&4
            if [[ -n "$LOGFILE" && -f "$LOGFILE" ]]; then
                echo -e "${RED}ERROR:${NO_COLOR} wrapper log output:" >&4
                cat "$LOGFILE" >&4
            fi
        fi

        # Also clean up here for early exits
        cleanup_logfile
    }
  
    trap 'print_summary' EXIT      
fi 

# find existing python installs
cur_dir=$(basename $REPO_ROOT)
python=""
if [ -d $REPO_ROOT/bazel-$cur_dir ]; then
  python="$(readlink "$REPO_ROOT/bazel-$cur_dir")/../../external/_main~setup_mongo_python_toolchains~py_${os}_${ARCH}/dist/bin/python3"
else
  if [ -d $REPO_ROOT/.compiledb/compiledb-$cur_dir ]; then
    python="$(readlink "$REPO_ROOT/.compiledb/compiledb-$cur_dir")/../../external/_main~setup_mongo_python_toolchains~py_${os}_${ARCH}/dist/bin/python3"
  fi
fi

# if no python use bazel to install one
if [[ "$python" = "" ]] || [ ! -f $python ]; then
  >&2 echo "python prereq missing, using bazel to install python..."
  >&2 $bazel_real build --bes_backend= --bes_results_url= --workspace_status_command= @py_${os}_${ARCH}//:all
  if [[ $? != 0 ]]; then
    >&2 $bazel_real build --config=local --workspace_status_command= @py_${os}_${ARCH}//:all
    if [[ $? != 0 ]]; then
      if [[ ! -z "$CI" ]] || [[ $MONGO_BAZEL_WRAPPER_FALLBACK == 1 ]]; then
        >&2 echo "wrapper script failed to install python! falling back to normal bazel call..."
        "$bazel_real" "$@"
        exit $?
      else
        exit $?
      fi
    fi
  fi
fi

# update python location if it was missing and we had to install it
if [[ "$python" = "" ]] || [ ! -f $python ]; then
  python="$(readlink "$REPO_ROOT/bazel-$cur_dir")/../../external/_main~setup_mongo_python_toolchains~py_${os}_${ARCH}/dist/bin/python3"
fi

autocomplete_query=0
# bash autocomplete detection
if [[ $* =~ ^.*--output_base=/tmp/.*-completion-$USER.*$ ]]; then
  autocomplete_query=1
fi
# zsh autocomplete detection
# Note that this is a little weird because it is a regex that matches another regex.
# The gist is that it looks for ".*_test" or ".*_test|.*_binary" with :all or //:all
# Bash has odd quoting rules for the RHS of =~, which are avoided by using a variable.
zsh_autocomplete_regex='--noblock_for_lock query kind\("\.\*_test(\|\.\*_binary)?", (//)?:all\)'
if [[ $* =~ $zsh_autocomplete_regex ]]; then
  autocomplete_query=1
fi

rm -f /tmp/mongo_autocomplete_plus_targets
MONGO_BAZEL_WRAPPER_ARGS=$(mktemp)

MONGO_BAZEL_WRAPPER_ARGS=$MONGO_BAZEL_WRAPPER_ARGS \
MONGO_AUTOCOMPLETE_QUERY=$autocomplete_query \
>&2 $python $REPO_ROOT/bazel/wrapper_hook/wrapper_hook.py $bazel_real "$@"
exit_code=$?
# Linter fails preempt bazel run.
if [[ $exit_code == 3 ]]; then
  RED='\033[0;31m'
  GREEN='\033[0;32m'
  NO_COLOR='\033[0m'
  echo -e "${RED}ERROR:${NO_COLOR} Linter run failed, see details above"
  echo -e "${GREEN}INFO:${NO_COLOR} Run the following to try to auto-fix the errors:\n\nbazel run lint --fix"
  exit $exit_code
fi
if [[ $exit_code != 0 ]]; then
  if [[ ! -z "$CI" ]] || [[ $MONGO_BAZEL_WRAPPER_FALLBACK == 1 ]]; then
    >&2 echo "wrapper script failed! falling back to normal bazel call..."
    "$bazel_real" "$@"
    exit $?
  else
    exit $? 
  fi
fi

new_args=()
while IFS= read -r line; do
  new_args+=("$line")
done < $MONGO_BAZEL_WRAPPER_ARGS

if [[ $MONGO_BAZEL_WRAPPER_DEBUG == 1 ]] && [[ $autocomplete_query == 0 ]]; then
  wrapper_end_time="$(date -u +%s.%N)"
  runtime=$(bc <<< "$wrapper_end_time - $wrapper_start_time")
  runtime=$(printf "%0.3f" $runtime)
  >&2 echo "[WRAPPER_HOOK_DEBUG]: wrapper hook script input args: $@"
  >&2 echo "[WRAPPER_HOOK_DEBUG]: wrapper hook script new args: ${new_args[@]@Q}"
  >&2 echo "[WRAPPER_HOOK_DEBUG]: wrapper hook script took $runtime seconds"
fi

if [[ $autocomplete_query == 1 ]]; then  
  plus_targets=$(</tmp/mongo_autocomplete_plus_targets)  
  query_output=$("${new_args[@]@Q}")  
  echo $query_output $plus_targets | tr " " "\n" >&3  
else  
  trap 'cleanup_logfile' EXIT
  # Slow path: restore stdout/stderr so Bazel prints normally  
  if [[ -z "${MONGO_WRAPPER_OUTPUT_ALL}" ]]; then
      exec 1>&3 2>&4
  fi
  
  $bazel_real "${new_args[@]}"  
  bazel_exit_code=$?  
  ( >&2 $python $REPO_ROOT/bazel/wrapper_hook/post_bazel_hook.py $bazel_real )  
  exit $bazel_exit_code  
fi 
