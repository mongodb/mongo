ENGFLOW_CLUSTER="sodalite.cluster.engflow.com"

# Ask the invocation page for every target status: without these filters it hides targets outside
# the current tree filter, the "#targets-" anchor included.
ENGFLOW_TREE_FILTERS="?treeFilterStatus=failed&treeFilterStatus=in+progress&treeFilterStatus=not+built&treeFilterStatus=succeeded"

# Prints the EngFlow invocation id (the BEP started event's uuid) for a build_events.json.
# Usage: engflow_links::invocation_id <bep file>
function engflow_links::invocation_id() {
    jq --raw-output 'select(.started != null) | .started.uuid' "$1" 2>/dev/null | head -n 1
}

# Emits one Evergreen attach.artifacts entry. do_not_encode_link keeps Evergreen from
# percent-escaping the last path segment of the link, which would destroy the query string and the
# "#targets-" anchor.
# Usage: engflow_links::entry <display name> <link>
function engflow_links::entry() {
    jq --null-input --compact-output --arg name "$1" --arg link "$2" \
        '{name: $name, link: $link, visibility: "public", ignore_for_fetch: true, do_not_encode_link: true}'
}

# Link to a whole invocation.
# Usage: engflow_links::invocation_url <invocation id>
function engflow_links::invocation_url() {
    printf 'https://%s/invocations/default/%s' "$ENGFLOW_CLUSTER" "$1"
}

# Link to a single target within an invocation.
# Usage: engflow_links::target_url <invocation id> <label>
function engflow_links::target_url() {
    printf 'https://%s/invocations/default/%s%s#targets-%s' \
        "$ENGFLOW_CLUSTER" "$1" "$ENGFLOW_TREE_FILTERS" "$(printf %s "$2" | base64 | tr -d '\n')"
}
