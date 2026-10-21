# Make every directory under the task working directory readable by the agent user.
#
# The Evergreen agent's generate.tasks command walks the whole working directory to expand its
# `files:` patterns and fails the task if it hits a directory it cannot open. Tests and
# containers (e.g. Copybara's docker run as root, or tests that chmod their temp dirs) can leave
# such directories behind.

sudo=
sudo -n date >/dev/null 2>&1 && sudo=sudo

# Best effort: never fail the task from here.
if [ -n "$sudo" ]; then
    $sudo chown -R "$(id -u):$(id -g)" . 2>/dev/null || true
fi
chmod -R u+rwX . 2>/dev/null || true

exit 0
