function dump_catalog() {
    local FILES_PATH="$1"
    if [ -d "$FILES_PATH" ]; then
        local wt_home_directory="$FILES_PATH"
    else
        local wt_home_directory=$(dirname "$FILES_PATH")
    fi
    local catalog_file="_mdb_catalog.wt"

    "$WORKSPACE_FOLDER/bazel-bin/install/bin/wt" -h "$wt_home_directory" -r dump -x "file:$catalog_file" |
        "$WORKSPACE_FOLDER/src/third_party/wiredtiger/tools/wt_to_mdb_bson.py" -m dump --json
}

function dump_collection() {
    local FILES_PATH="$1"
    local COLLECTION_NAME="$2"

    if [ -d "$FILES_PATH" ]; then
        local wt_home_directory="$FILES_PATH"
    else
        local wt_home_directory=$(dirname "$FILES_PATH")
    fi
    local catalog_file="_mdb_catalog.wt"

    local collection_file
    collection_file="$(dump_catalog "$FILES_PATH" | jq --arg ns "$COLLECTION_NAME" -r '.value | select(.ns == $ns) | .ident')"

    if [[ -z "$collection_file" || "$collection_file" == "null" ]]; then
        echo "Error: collection namespace not found in catalog: $COLLECTION_NAME" >&2
        return 1
    fi
    if [[ "$COLLECTION_NAME" == local.* ]]; then
        local extra_wt_options=(-R -C 'log=(enabled=true,path=journal,compressor=snappy)')
    else
        local extra_wt_options=(-r)
    fi

    "$WORKSPACE_FOLDER/bazel-bin/install/bin/wt" -h "$wt_home_directory" "${extra_wt_options[@]}" dump -x "file:$collection_file.wt" |
        "$WORKSPACE_FOLDER/src/third_party/wiredtiger/tools/wt_to_mdb_bson.py" -m dump --json

}

if [[ -n ${BASH_VERSION-} ]]; then
    function _dump_collection() {
        if [[ "${COMP_CWORD}" -eq 1 ]]; then
            COMPREPLY=($(compgen -o dirnames -- "${COMP_WORDS[COMP_CWORD]}"))
            return 0
        fi
        if [[ "${COMP_CWORD}" -eq 2 ]]; then
            local FILES_PATH="${COMP_WORDS[COMP_CWORD - 1]}"
            local coll_ns
            coll_ns="$(dump_catalog "$FILES_PATH" | jq -r '.value.ns' | tr '\n' ' ')"
            local cur="${COMP_WORDS[COMP_CWORD]}"
            COMPREPLY=($(compgen -W "${coll_ns}" -- "${cur}"))
        fi
    }
    complete -F _dump_collection dump_collection
fi
