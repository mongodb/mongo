function setup_db_contrib_tool_venv {

  mkdir ${workdir}/pipx
  export PIPX_HOME="${workdir}/pipx"
  export PIPX_BIN_DIR="${workdir}/pipx/bin"
  export PATH="$PATH:$PIPX_BIN_DIR"

  python -m pip --disable-pip-version-check install "pip==21.0.1" "wheel==0.37.0" || exit 1
  python -m pip --disable-pip-version-check install "pipx" || exit 1
  pipx install "db-contrib-tool==0.4.5" || exit 1
}
