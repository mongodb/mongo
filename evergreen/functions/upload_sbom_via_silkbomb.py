import pathlib
import subprocess
import sys

import typer
from git import Repo
from typing_extensions import Annotated

app = typer.Typer(
    help="Checks for SBOM file changes in a PR and uploads it to Kondukto if changed.",
    add_completion=False,
)


def get_changed_files_from_latest_commit(local_repo_path: str, branch_name: str = "master") -> dict:
    try:
        repo = Repo(local_repo_path)

        if branch_name not in repo.heads:
            raise ValueError(f"Branch '{branch_name}' does not exist in the repository.")

        last_commit = repo.heads[branch_name].commit
        title = last_commit.summary
        commit_hash = last_commit.hexsha

        # If the last commit has no parents, it means it's the first commit in the repo
        if not last_commit.parents:
            files = [item.path for item in last_commit.tree.traverse()]
        else:
            # Comparing the last commit with its parent to find changed files
            files = [file.a_path for file in last_commit.diff(last_commit.parents[0])]

        return {"title": title, "hash": commit_hash, "files": files}
    except Exception as e:
        print(f"Error retrieving changed files: {e}")
        raise e


def upload_sbom_via_silkbomb(
    sbom_repo_path: str,
    workdir: str,
    local_repo_path: str,
    repo_name: str,
    branch_name: str,
    creds_file_path: pathlib.Path,
    container_command: str,
    container_image: str,
    timeout_seconds: int = 60 * 5,
):
    container_options = ["--pull=always", "--platform=linux/amd64", "--rm"]
    container_env_files = ["--env-file", str(creds_file_path.resolve())]
    container_volumes = ["-v", f"{workdir}:/workdir"]
    silkbomb_command = "augment"  # it augment first and uses upload command
    silkbomb_args = [
        "--sbom-in",
        f"/workdir/{local_repo_path}/{sbom_repo_path}",
        "--branch",
        branch_name,
        "--repo",
        repo_name,
    ]

    command = [
        container_command,
        "run",
        *container_options,
        *container_env_files,
        *container_volumes,
        container_image,
        silkbomb_command,
        *silkbomb_args,
    ]

    aws_region = "us-east-1"
    ecr_registry_url = (
        "901841024863.dkr.ecr.us-east-1.amazonaws.com/release-infrastructure/silkbomb"
    )

    print(f"Attempting to authenticate to AWS ECR registry '{ecr_registry_url}'...")
    try:
        login_cmd = f"aws ecr get-login-password --region {aws_region} | {container_command} login --username AWS --password-stdin {ecr_registry_url}"
        subprocess.run(
            login_cmd,
            shell=True,
            check=True,
            text=True,
            capture_output=True,
            timeout=timeout_seconds,
        )
        print("ECR authentication successful.")
    except FileNotFoundError:
        print(
            f"Error: A required command was not found. Please ensure AWS CLI and '{container_command}' are installed and in your PATH."
        )
        raise
    except subprocess.TimeoutExpired as e:
        print(
            f"Error: Command timed out after {timeout_seconds} seconds. Please check Evergreen network state and try again."
        )
        raise e
    except subprocess.CalledProcessError as e:
        print(f"Error during ECR authentication:\n--- STDERR ---\n{e.stderr}")
        raise

    try:
        print(f"Running command: {' '.join(command)}")
        subprocess.run(command, check=True, text=True, capture_output=True, timeout=timeout_seconds)
        print("Updated sbom.json file upload via Silkbomb successful!")
    except FileNotFoundError as e:
        print(f"Error: '{container_command}' command not found.")
        raise e
    except subprocess.TimeoutExpired as e:
        print(
            f"Error: Command timed out after {timeout_seconds} seconds. Please check Evergreen network state and try again."
        )
        raise e
    except subprocess.CalledProcessError as e:
        print(
            f"Error during container execution:\n--- STDOUT ---\n{e.stdout}\n--- STDERR ---\n{e.stderr}"
        )
        raise e


# TODO (SERVER-109205): Add Slack Alerts for failures
@app.command()
def run(
    github_org: Annotated[
        str,
        typer.Option(..., envvar="GITHUB_ORG", help="Name of the github organization (e.g. 10gen)"),
    ],
    github_repo: Annotated[
        str, typer.Option(..., envvar="GITHUB_REPO", help="Repo name in 'owner/repo' format.")
    ],
    local_repo_path: Annotated[
        str,
        typer.Option(..., envvar="LOCAL_REPO_PATH", help="Path to the local git repository."),
    ],
    branch_name: Annotated[
        str,
        typer.Option(..., envvar="BRANCH_NAME", help="The head branch (e.g., the PR branch name)."),
    ],
    sbom_repo_path: Annotated[
        str,
        typer.Option(
            ...,
            "--sbom-in",
            envvar="SBOM_REPO_PATH",
            help="Path to the SBOM file to check and upload.",
        ),
    ] = "sbom.json",
    requester: Annotated[
        str,
        typer.Option(
            ...,
            envvar="REQUESTER",
            help="The entity requesting the run (e.g., 'github_merge_queue').",
        ),
    ] = "",
    container_command: Annotated[
        str,
        typer.Option(
            ..., envvar="CONTAINER_COMMAND", help="Container engine to use ('podman' or 'docker')."
        ),
    ] = "podman",
    container_image: Annotated[
        str, typer.Option(..., envvar="CONTAINER_IMAGE", help="Silkbomb container image.")
    ] = "901841024863.dkr.ecr.us-east-1.amazonaws.com/release-infrastructure/silkbomb:2.0",
    creds_file: Annotated[
        pathlib.Path,
        typer.Option(
            ..., envvar="CONTAINER_ENV_FILES", help="Path for the temporary credentials file."
        ),
    ] = pathlib.Path("kondukto_credentials.env"),
    workdir: Annotated[
        str, typer.Option(..., envvar="WORKING_DIR", help="Path for the container volumes.")
    ] = "/workdir",
    dry_run: Annotated[
        bool, typer.Option("--dry-run/--run", help="Check for changes without uploading.")
    ] = True,
    check_sbom_file_change: Annotated[
        bool, typer.Option("--check-sbom-file-change", help="Check for changes to the SBOM file.")
    ] = False,
):
    if requester != "commit" and not dry_run:
        print(f"Skipping: Run can only be triggered for 'commit', but requester was '{requester}'.")
        sys.exit(0)

    major_branches = ["v7.0", "v8.0", "v8.1", "master"]  # Only major branches that MongoDB supports
    if False and branch_name not in major_branches:
        print(f"Skipping: Branch '{branch_name}' is not a major branch. Exiting.")
        sys.exit(0)

    repo_path = pathlib.Path(f"{workdir}/{local_repo_path}")
    sbom_path = pathlib.Path(f"{repo_path}/{sbom_repo_path}")
    if not sbom_path.resolve().exists():
        print(f"Error: SBOM file not found at path: {str(sbom_path.resolve())}")
        sys.exit(1)

    try:
        sbom_file_changed = True
        if check_sbom_file_change:
            commit_changed_files = get_changed_files_from_latest_commit(repo_path, branch_name)
            if commit_changed_files:
                print(
                    f"Latest commit '{commit_changed_files['title']}' ({commit_changed_files['hash']}) in branch '{branch_name}' has the following changed files:"
                )
                print(f"{commit_changed_files['files']}")
            else:
                print(
                    f"No changed files found in the commit '{commit_changed_files['title']}' ({commit_changed_files['hash']}) in branch '{branch_name}'. Exiting without upload."
                )
                sys.exit(0)

            print(f"Checking for changes to file: {sbom_path} ({sbom_repo_path})")

            sbom_file_changed = sbom_repo_path in commit_changed_files["files"]

            if sbom_file_changed:
                print(f"File '{sbom_path}' was modified. Initiating upload...")
            else:
                print(f"File '{sbom_repo_path}' was not modified. Nothing to upload.")

        if not dry_run and sbom_file_changed:
            upload_sbom_via_silkbomb(
                sbom_repo_path=sbom_repo_path,
                workdir=workdir,
                local_repo_path=local_repo_path,
                repo_name=f"{github_org}/{github_repo}",
                branch_name=branch_name,
                creds_file_path=creds_file,
                container_command=container_command,
                container_image=container_image,
            )
        else:
            print("--dry-run enabled, skipping upload.")
            print(
                f"File '{sbom_repo_path}'"
                + (" was modified." if sbom_file_changed else " was not modified.")
            )

        if dry_run:
            print("Upload metadata:")
            print(f"  SBOM Path: {sbom_path}")
            print(f"  Repo Name: '{github_org}/{github_repo}'")
            print(f"  Repo Branch: '{branch_name}'")
            print(f"  Container Command: {container_command}")
            print(f"  Container Image: {container_image}")
            print(f"  Workdir: {workdir}")
            if check_sbom_file_change:
                print(
                    f"Latest commit '{commit_changed_files['title']}' ({commit_changed_files['hash']})"
                )

    except Exception as e:
        print(f"Exception during script execution: {e}")
        sys.exit(1)


if __name__ == "__main__":
    app()
