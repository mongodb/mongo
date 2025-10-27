#!/bin/bash
# xdg-open wrapper for devcontainer
# Launches URLs using VSCode's $BROWSER environment variable
# Falls back to native xdg-open for non-URLs

NATIVE_XDG_OPEN="/usr/bin/xdg-open.real"

# If it's an HTTP/HTTPS URL and $BROWSER is set, use it
if [[ "$1" == http:* || "$1" == https:* ]] && [ -n "$BROWSER" ]; then
    exec "$BROWSER" "$@"
fi

# Otherwise, fall back to native xdg-open if it exists
if [ -x "$NATIVE_XDG_OPEN" ]; then
    exec "$NATIVE_XDG_OPEN" "$@"
fi

# If we get here, we couldn't handle it
echo "Error: Cannot open '$1' - no suitable handler found" >&2
exit 1
