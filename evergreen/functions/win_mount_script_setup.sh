DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/../prelude.sh"

cd src

cat << EOF > win_mount.sh
net use X: '\\\\${win_scons_endpoint}\\share' /USER:"wincache.build.com\\${win_scons_user}" '${win_scons_pass}'
EOF
chmod +x win_mount.sh
