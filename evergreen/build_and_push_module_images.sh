set -e

for dir in ./src/src/mongo/db/modules/*; do
    if test -f $dir/evergreen/build_and_push_images.sh; then
        bash $dir/evergreen/build_and_push_images.sh
    fi
done
