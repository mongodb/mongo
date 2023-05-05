#!/bin/bash

# Notes on how to run this manually:
# - repo must be unpacked into source tree
#
# export ssh_key=$HOME/.ssh/id_rsa
# export hostname=ec2-3-91-230-150.compute-1.amazonaws.com
# export user=ec2-user
# export bypass_prelude=yes
# export workdir="$(dirname $(pwd) | tee /dev/stderr)"
# export src="$(basename $(pwd) | tee /dev/stderr)"
# export test_list='jstests/selinux/*.js'
# export pkg_variant=mongodb-enterprise
# evergreen/selinux_run_test.sh

set -o errexit

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
if [ "$bypass_prelude" != "yes" ]; then
  . "$DIR/prelude.sh"
  activate_venv
  src="src"
fi

if [ "$hostname" == "" ]; then
  hostname="$(tr -d '"[]{}' < "$workdir"/$src/hosts.yml | cut -d , -f 1 | awk -F : '{print $2}')"
fi

if [ "$user" == "" ]; then
  user=$USER
fi

host="${user}@${hostname}"
python="${python:-python3}"

if [ "$ssh_key" == "" ]; then
  ssh_key="$workdir/selinux.pem"
  "$workdir"/$src/buildscripts/yaml_key_value.py --yamlFile="$workdir"/expansions.yml \
    --yamlKey=__project_aws_ssh_key_value > "$ssh_key"
  chmod 600 "$ssh_key"
  result="$(openssl rsa -in "$ssh_key" -check -noout | tee /dev/stderr)"
  if [ "$result" != "RSA key ok" ]; then
    exit 1
  fi
fi

attempts=0
connection_attempts=50

# Check for remote connectivity
set +o errexit
ssh_options="-i $ssh_key -o IdentitiesOnly=yes -o StrictHostKeyChecking=no"
while ! ssh -q $ssh_options -o ConnectTimeout=10 "$host" echo "I am working"; do
  if [ "$attempts" -ge "$connection_attempts" ]; then
    printf "SSH connection attempt failed after %d attempts.\n" "$attempts"
    exit 1
  fi
  ((attempts++))
  sleep 10
done

set -o errexit
echo "===> Copying sources to target..."
rsync -ar -e "ssh $ssh_options" \
  --exclude 'tmp' --exclude 'build' --exclude '.*' \
  "$workdir"/$src/* "$host":

echo "===> Configuring target machine..."
ssh $ssh_options "$host" evergreen/selinux_test_setup.sh

echo "===> Executing tests..."
list="$(
  cd src
  for x in $test_list; do echo "$x"; done
)"
for test in $list; do
  ssh $ssh_options "$host" evergreen/selinux_test_executor.sh "$test"
done
