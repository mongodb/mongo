DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

cd src

set -o errexit
cat << EOF > aws_e2e_setup.json
{
    "iam_auth_ecs_account" : "${iam_auth_ecs_account}",
    "iam_auth_ecs_secret_access_key" : "${iam_auth_ecs_secret_access_key}",
    "iam_auth_ecs_account_arn": "arn:aws:iam::557821124784:user/authtest_fargate_user",
    "iam_auth_ecs_cluster": "${iam_auth_ecs_cluster}",
    "iam_auth_ecs_task_definition": "${iam_auth_ecs_task_definition}",
    "iam_auth_ecs_subnet_a": "${iam_auth_ecs_subnet_a}",
    "iam_auth_ecs_subnet_b": "${iam_auth_ecs_subnet_b}",
    "iam_auth_ecs_security_group": "${iam_auth_ecs_security_group}",

    "iam_auth_assume_aws_account" : "${iam_auth_assume_aws_account}",
    "iam_auth_assume_aws_secret_access_key" : "${iam_auth_assume_aws_secret_access_key}",
    "iam_auth_assume_role_name" : "${iam_auth_assume_role_name}",

    "iam_auth_ec2_instance_account" : "${iam_auth_ec2_instance_account}",
    "iam_auth_ec2_instance_secret_access_key" : "${iam_auth_ec2_instance_secret_access_key}",
    "iam_auth_ec2_instance_profile" : "${iam_auth_ec2_instance_profile}"
}
EOF
