#!/bin/bash

# Create a git hook driver in this repo. The hook will run all
# excutable scripts in the directory ~/.githooks/<repo name>/[HOOK_TRIGGER]/
# where HOOK_TRIGGER can be any one of the following:
# https://git-scm.com/docs/githooks#_hooks
# If you add a new type of HOOK_TRIGGER, this script needs to be run again.

# Find out the repo name and where the .git directory is for this repo
origin="$(git config remote.origin.url)"
repo="$(basename -s .git $origin)"
tld="$(git rev-parse --show-toplevel)"

# Location for the hooks.
# WARNING: if you change this you'll need to change the value of the
# "hooks_dir" variable in the heredoc below as well
hooks_dir="$HOME/.githooks/$repo"

tmp_hook=$(mktemp)

usage() {
    echo "Usage: $(basename $0) [-f|-h]"
    echo
    echo "  git hooks in $tld/.git/hooks that run scripts in $hooks_dir"
    echo "  -f force overwriting existing hooks"
    echo "  -h print this help"
    echo
    return
}

if [ ! -d "$hooks_dir" ]; then
  # Bail out if the githooks directory doesn't exist.
  echo "Place your scripts in the directory $hooks_dir/[HOOK_TRIGGER]/ where [HOOK_TRIGGER] is the"
  echo "name of the git hook that you want your script to run under, like 'pre-push' or 'pre-commit'"
  echo
  echo "See the full list of git hooks here: https://git-scm.com/docs/githooks#_hooks"
  echo
  echo "Once you have placed your scripts, re-run this file."
  echo
  return
fi

# --- Command line options ---
force=0
while getopts fh opt_arg; do
    case $opt_arg in
        f) force=1 ;;
        *) usage   ;;
    esac
done
shift $(expr $OPTIND - 1)
# ----------------------------

set -e

cat > $tmp_hook <<'EOF'
#!/bin/bash

# set GITHOOKS_QUIET to anything to anything to make this script quiet
quiet=${GITHOOKS_QUIET:-""}
[ -z "$quiet" ] && echo -e "[INFO] Starting up hook runner ..."

origin="$(git config remote.origin.url)"
repo="$(basename -s .git $origin)"
this_script=$(basename "$0")

hooks_dir=$HOME/.githooks/$repo/$this_script
if [ ! -d "$hooks_dir" ]; then
    echo "[WARNING] Hooks directory doesn't exist: $hooks_dir. Running $this_script anyway"
    sleep 2
    exit 0
fi

# Only run if the remote URL matches the MongoDB repo.
url=$2
if [ "$this_script" == "pre-push" ] && ! [[ $url =~ (mongodb|10gen)/ ]]; then
    echo "Skipping pre-push hook for non-MongoDB remote URL: $url"
    exit 0
fi

num_hooks_run=0
all_hooks="$(/bin/ls -1 $hooks_dir 2>/dev/null)"

for hook_name in $(echo $all_hooks) ; do
    hook_path=$hooks_dir/$hook_name

    if [ -e "$hook_path" ]; then
        [ -z "$quiet" ] && echo -e "[INFO] Running hook: $hook_path"
        $hook_path "$@"
        
        hook_status=$?
        num_hooks_run=$((num_hooks_run + 1))
        
        if [ $hook_status -ne 0 ]; then
            echo "[ERROR] Hook $hook_path returned non-zero status: $hook_status"
            exit $hook_status
        else
            [ -z "$quiet" ] && echo "[INFO] Done."
        fi
    fi
done

if [ $num_hooks_run -eq 0 ]; then
    echo "[WARNING] Couldn't find any hooks to run in $hooks_dir. Running $this_script anyway"
    sleep 2
fi

exit 0
EOF

hook_types=($(ls -d $hooks_dir/*))

for cur_hook_full_path in "${hook_types[@]}"
do
    cur_hook_type=$(basename $cur_hook_full_path)
    cur_hook_path="$tld/.git/hooks/$cur_hook_type"

    echo "Installing $cur_hook_type hook in $cur_hook_path ..."

    # If there's already a hook installed, bail out. We don't want to overwrite it.
    # (unless -f is passed in the command line)
    if [ -e "$cur_hook_path" -a $force -eq 0 ]; then
        echo "[ERROR] Found an existing $cur_hook_type hook: $cur_hook_path"
        exit 1
    fi

    cp $tmp_hook $cur_hook_path
    chmod +x $cur_hook_path
done

rm $tmp_hook
echo "Done."
