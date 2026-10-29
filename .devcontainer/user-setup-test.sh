#!/usr/bin/env bash

# Run with bash .devcontainer/user-setup-test.sh. Requires Docker.
set -euo pipefail

devcontainer_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
base_image=$(sed -n 's/^ARG BASE_IMAGE=//p' "$devcontainer_dir/Dockerfile")
user_setup=$(awk '
    /^# Create the user/ { capture = 1; next }
    capture && /^$/ { exit }
    capture { sub(/^RUN /, ""); print }
' "$devcontainer_dir/Dockerfile")
test -n "$user_setup"

for scenario in existing-user new-user existing-group; do
    echo "Testing devcontainer user setup: $scenario"
    docker run --rm --entrypoint /bin/bash \
        --env "USER_SETUP=$user_setup" \
        --env "SCENARIO=$scenario" \
        "$base_image" -euo pipefail -c '
            case "$SCENARIO" in
                existing-user)
                    export USERNAME=ubuntu
                    original_uid=$(id -u "$USERNAME")
                    original_gid=$(id -g "$USERNAME")
                    touch "/home/$USERNAME/preserve-existing-home"
                    ;;
                new-user)
                    export USERNAME=mongo-dev
                    ;;
                existing-group)
                    export USERNAME=mongo-dev
                    groupadd "$USERNAME"
                    ;;
            esac

            eval "$USER_SETUP"
            test "$(id -gn "$USERNAME")" = "$USERNAME"
            test "$(getent passwd "$USERNAME" | cut -d: -f6)" = "/home/$USERNAME"
            test "$(getent passwd "$USERNAME" | cut -d: -f7)" = /bin/bash
            test "$(stat -c %u "/home/$USERNAME")" = "$(id -u "$USERNAME")"
            if [ "$SCENARIO" = existing-user ]; then
                test "$(id -u "$USERNAME")" = "$original_uid"
                test "$(id -g "$USERNAME")" = "$original_gid"
                test -f "/home/$USERNAME/preserve-existing-home"
            fi
        '
done
