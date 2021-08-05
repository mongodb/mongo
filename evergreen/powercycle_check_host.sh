DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

if [ "Windows_NT" = "$OS" ]; then
  user=Administrator
else
  user=$USER
fi
hostname=$(tr -d '"[]{}' < src/hosts.yml | cut -d , -f 1 | awk -F : '{print $2}')

# To add the hostname to expansions.
echo "private_ip_address: $hostname" >> src/powercycle_ip_address.yml

echo $hostname
echo $user

attempts=0
connection_attempts=60

# Check for remote connectivity
while ! ssh \
  -i ${private_key_file} \
  -o ConnectTimeout=10 \
  -o ForwardAgent=yes \
  -o IdentitiesOnly=yes \
  -o StrictHostKeyChecking=no \
  "$(printf "%s@%s" "$user" "$hostname")" \
  exit 2> /dev/null; do
  [ "$attempts" -ge "$connection_attempts" ] && exit 1
  ((attempts++))
  printf "SSH connection attempt %d/%d failed. Retrying...\n" "$attempts" "$connection_attempts"
  # sleep for Permission denied (publickey) errors
  sleep 10
done
