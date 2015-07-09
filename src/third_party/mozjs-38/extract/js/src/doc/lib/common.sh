# Common utility functions for js/src/doc scripts.

# `relative BASE ABSOLUTE` prints the URL relative to BASE that is
# equivalent to ABSOLUTE. BASE must end with a '/'. This function will
# introduce at most one level of '..'.
relative() {
    local parent=$(dirname "$1")
    case "$2" in
        "$1"*)
            # ABSOLUTE is within BASE; just remove BASE.
            echo "$2" | sed -e "s|^$1||"
            ;;
        "$parent/"*)
            # ABSOLUTE is within BASE/..
            echo "$2" | sed -e "s|^$parent/|../|"
            ;;
        *)
            # ABSOLUTE is unrelated to BASE.
            echo "$2"
            ;;
    esac
}

