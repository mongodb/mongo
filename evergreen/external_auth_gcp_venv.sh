function setup_gcp_venv {
    # GCP dependencies must not replace the locked packages used by resmoke.
    # Isolated mode also ignores the shared site-packages in Evergreen's PYTHONPATH.
    # Keep the helper environment under venv so existing task cleanup removes it.
    local gcp_venv="${workdir}/venv/gcp-oidc"
    "$python" -I -m venv "$gcp_venv"
    gcp_python="${gcp_venv}/bin/python"
    "$gcp_python" -I -m pip install -r evergreen/external_auth_gcp_requirements.txt
}
