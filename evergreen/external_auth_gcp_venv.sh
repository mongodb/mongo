function setup_gcp_venv {
    # GCP dependencies must not replace the locked packages used by resmoke.
    # Isolated mode also ignores the shared site-packages in Evergreen's PYTHONPATH.
    # Keep the helper environment under venv so existing task cleanup removes it.
    local gcp_venv="${workdir}/venv/gcp-oidc"
    "$python" -I -m venv "$gcp_venv"
    gcp_python="${gcp_venv}/bin/python"
    # --require-hashes enforces the sha256 hashes in the requirements file, so
    # nothing outside the repository's pinned artifacts can be installed.
    # --only-binary=:all: additionally refuses source distributions, since
    # --require-hashes does not cover the build dependencies pip downloads for
    # PEP 517 build isolation when building an sdist.
    "$gcp_python" -I -m pip install --require-hashes --only-binary=:all: -r evergreen/external_auth_gcp_requirements.txt
}
