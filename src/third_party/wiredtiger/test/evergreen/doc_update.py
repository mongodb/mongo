#!/usr/bin/env python3
#
# This script attempts to generate Github Apps token and use it to push a local commit to remote.
# It assumes the local commit is ready and the below set of environment variables are set:
# - GITHUB_OWNER
# - GITHUB_REPO
# - GITHUB_APP_ID
# - GITHUB_APP_PRIVATE_KEY
#

import os
import subprocess
import sys

from github.GithubIntegration import GithubIntegration


# Get the App
app_id = os.getenv("GITHUB_APP_ID")
private_key = os.getenv("GITHUB_APP_PRIVATE_KEY")
if (not app_id) or (not private_key):
    sys.exit("Please ensure GITHUB_APP_ID and GITHUB_APP_PRIVATE_KEY environment variables are set.")
try:
    app = GithubIntegration(int(app_id), private_key)
except Exception as e:
    sys.exit(e)

# Get the installation
owner = os.getenv("GITHUB_OWNER")
repo = os.getenv("GITHUB_REPO")
if (not owner) or (not repo):
    sys.exit("Please ensure GITHUB_OWNER and GITHUB_REPO environment variables are set.")
try:
    installation = app.get_repo_installation(owner, repo)
    print(f"{installation.id=} for {owner}/{repo}")
except Exception as e:
    sys.exit(e)

# Get an installation access token
try:
    installation_auth = app.get_access_token(installation.id)
    # Print the token to STDOUT and save it in an environment variable
    os.environ["GITHUB_APP_INSTALLATION_TOKEN"] = installation_auth.token
except Exception as e:
    sys.exit(e)

# Use the token to push the commit
try:
    with os.chdir('wiredtiger.github.com'):
        cmd = f"git push https://'{app_id}:{installation_auth.token}'@github.com/{owner}/{repo}"
        subprocess.run([cmd], shell=True, check=True)
except Exception as e:
    sys.exit(e)
