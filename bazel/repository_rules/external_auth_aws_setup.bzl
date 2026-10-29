"""Generates aws_e2e_setup.json, the credential file the external_auth_aws jstests read."""

_IAM_AUTH_ECS_ACCOUNT_ARN = "arn:aws:iam::557821124784:user/authtest_fargate_user"

_EVERGREEN_EXPANSIONS = [
    "iam_auth_ecs_account",
    "iam_auth_ecs_secret_access_key",
    "iam_auth_ecs_cluster",
    "iam_auth_ecs_task_definition",
    "iam_auth_ecs_subnet_a",
    "iam_auth_ecs_subnet_b",
    "iam_auth_ecs_security_group",
    "iam_auth_assume_aws_account",
    "iam_auth_assume_aws_secret_access_key",
    "iam_auth_assume_role_name",
]

_BUILD_FILE_CONTENT = """\
package(default_visibility = ["//visibility:public"])

exports_files(["aws_e2e_setup.json"])
"""

def _external_auth_aws_setup_impl(ctx):
    values = {name: ctx.os.environ.get(name, "") for name in _EVERGREEN_EXPANSIONS}
    values["iam_auth_ecs_account_arn"] = _IAM_AUTH_ECS_ACCOUNT_ARN

    ctx.file("BUILD.bazel", _BUILD_FILE_CONTENT)
    ctx.file("aws_e2e_setup.json", json.encode_indent(values, indent = "    ") + "\n")

external_auth_aws_setup = repository_rule(
    implementation = _external_auth_aws_setup_impl,
    doc = "Writes aws_e2e_setup.json from the iam_auth_* Evergreen expansions.",
    environ = _EVERGREEN_EXPANSIONS,
    local = True,
)
