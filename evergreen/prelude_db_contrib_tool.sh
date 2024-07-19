function setup_db_contrib_tool {

  mkdir -p ${workdir}/pipx
  export PIPX_HOME="${workdir}/pipx"
  export PIPX_BIN_DIR="${workdir}/pipx/bin"
  export PATH="$PATH:$PIPX_BIN_DIR"
  export PIP_CACHE_DIR=${workdir}/pip_cache

  python -m pip --disable-pip-version-check install "pip==21.0.1" "wheel==0.37.0" || exit 1
  # We force reinstall here because when we download the previous venv the shebang
  # in pipx still points to the old machines python location.
  python -m pip --disable-pip-version-check install --force-reinstall --no-deps "pipx==1.2.0" || exit 1
  pipx install "db-contrib-tool==0.7.0" --pip-args="--no-cache-dir" || exit 1
}
