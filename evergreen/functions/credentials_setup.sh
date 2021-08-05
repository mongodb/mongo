DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/../prelude.sh"

cd src

cat > mci.buildlogger << END_OF_CREDS
slavename='${slave}'
passwd='${passwd}'
builder='MCI_${build_variant}'
build_num=${builder_num}
build_phase='${task_name}_${execution}'
END_OF_CREDS
