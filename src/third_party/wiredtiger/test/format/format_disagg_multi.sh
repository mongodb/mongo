#!/bin/bash

[ -z "$BASH_VERSION" ] && {
    echo "$0 is a bash script: \$BASH_VERSION not set, exiting"
    exit 1
}

name=$(basename $0)
msg() { echo "$name: $@"; }
fatal_msg() { msg "$@"; exit 1; }
onintr() { fatal_msg "interrupted"; }
trap 'onintr' 2

usage() {
    echo "usage: $0"
    echo "    -h home      run directory (defaults to RUNDIR)"
    exit 1
}

home_dir="RUNDIR"

while :; do
    case $1 in
    -h | --home)
        home_dir="$2"
        shift ; shift ;;
    -?*)
        usage
        ;;
    *)
        break
        ;;
    esac
done

if [ $# -ne 0 ]; then
    echo "Unknown argument(s): $*" >&2
    usage
fi

PROG="./t"
CONFIG="../../../test/format/CONFIG.disagg"
DISAGG_MULTI_CONFIG="disagg.multi=1 disagg.multi_validation=1 runs.predictable_replay=1 runs.rows=100000:500000 runs.ops=5000000:20000000 "
LEADER_LOG="$home_dir/leader.out"
FOLLOWER_LOG="$home_dir/follower/follower.out"
SESSION_NAME="format_disagg_multi_node"

# Basic color escape sequences
GREEN=$(tput setaf 2)
CYAN=$(tput setaf 6)
RESET=$(tput sgr0)

# Check tmux
command -v tmux >/dev/null 2>&1 || fatal_msg "tmux not found (install via apt/brew)"

# Clean old session
tmux has-session -t "$SESSION_NAME" 2>/dev/null && tmux kill-session -t "$SESSION_NAME"

msg "Starting run in tmux session: $SESSION_NAME"
msg "Database directory: $home_dir"

# Run the test in background
tmux new-session -d -s "$SESSION_NAME" -n "run" \
    "clear; \
     echo '${CYAN}Running: $PROG -h $home_dir -c $CONFIG $DISAGG_MULTI_CONFIG $LIMIT_CONFIG${RESET}'; echo; \
     $PROG -h $home_dir -c $CONFIG $DISAGG_MULTI_CONFIG $LIMIT_CONFIG; \
     echo; \
     echo '${GREEN}Program finished. Press Ctrl-c :kill-session to close.${RESET}'; \
     read -p 'Press Enter to exit...'"

# Leader log pane
tmux new-window -t "$SESSION_NAME" -n "logs" \
    "bash -c 'while [ ! -f \"$LEADER_LOG\" ]; do sleep 0.2; done; \
     clear; \
     tail -n +1 -F \"$LEADER_LOG\"'"

# Follower log pane
tmux split-window -h -t "$SESSION_NAME:logs" \
    "bash -c 'while [ ! -f \"$FOLLOWER_LOG\" ]; do sleep 0.2; done; \
     clear; \
     tail -n +1 -F \"$FOLLOWER_LOG\"'"

tmux set-option -g mouse on # Mouse clicks the active pane
tmux select-layout -t "$SESSION_NAME:logs" even-horizontal
tmux select-pane -t "$SESSION_NAME:logs".0 -T "Leader Log"
tmux select-pane -t "$SESSION_NAME:logs".1 -T "Follower Log"
tmux set -g pane-border-status top
tmux set -g pane-border-format '#{pane_title}'
tmux set -g pane-active-border-style 'fg=green'
tmux set -g pane-border-style 'fg=yellow'

tmux set-option -g status off

msg "Logs ready. Attaching to tmux..."
tmux attach -t "$SESSION_NAME"
