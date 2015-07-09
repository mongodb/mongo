# Dummy definitions of functions used in doc directory config.sh files.
# Scripts reading config.sh files should source this first, and then
# override the definitions of the functions they care about.

base-url() {
    BASE_URL=$1
}

markdown() {
    INPUT_FILE=$1
    RELATIVE_URL=$2
}

label() {
    :
}

absolute-label() {
    :
}

resource() {
    :
}
