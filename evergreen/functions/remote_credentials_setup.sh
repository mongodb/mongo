DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/../prelude.sh"

# Since the macros 'private_key_remote' and 'private_key_file' are not always defined
# we default to /dev/null to avoid syntax errors of an empty expansion.
if [ -n "$private_key_remote_bash_var" ]; then
  private_key_remote="$private_key_remote_bash_var"
fi
if [ ! -z "${private_key_remote}" ] && [ ! -z "${private_key_file}" ]; then
  mkdir -p ~/.ssh
  private_key_file=$(eval echo "$private_key_file")
  echo -n "${private_key_remote}" > ${private_key_file}
  chmod 0600 ${private_key_file}
fi

# Ensure a clean aws configuration state
rm -rf ~/.aws
mkdir -p ~/.aws

# If ${aws_profile_remote} is not specified then the config & credentials are
# stored in the 'default' profile.
aws_profile="${aws_profile_remote}"

# The profile in the config file is specified as [profile <profile>], except
# for [default], see http://boto3.readthedocs.io/en/latest/guide/configuration.html
if [ $aws_profile = "default" ]; then
  aws_profile_config="[default]"
else
  aws_profile_config="[profile $aws_profile]"
fi
cat << EOF >> ~/.aws/config
$aws_profile_config
region = us-east-1
EOF

# The profile in the credentials file is specified as [<profile>].
cat << EOF >> ~/.aws/credentials
[$aws_profile]
aws_access_key_id = ${aws_key_remote}
aws_secret_access_key = ${aws_secret_remote}
EOF

cat << EOF > ~/.boto
[Boto]
https_validate_certificates = False
EOF
