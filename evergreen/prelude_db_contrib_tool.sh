function setup_db_contrib_tool {

  mkdir -p ${workdir}/pipx
  export PIPX_HOME="${workdir}/pipx"
  export PIPX_BIN_DIR="${workdir}/pipx/bin"
  export PATH="$PATH:$PIPX_BIN_DIR"

  for i in {1..5}; do
    python -m pip --disable-pip-version-check install "pip==21.0.1" "wheel==0.37.0" && RET=0 && break || RET=$? && sleep 1
    echo "Failed to install pip and wheel, retrying..."
  done

  if [ $RET -ne 0 ]; then
    echo "Failed to install pip and wheel"
    exit $RET
  fi

  for i in {1..5}; do
    # We force reinstall here because when we download the previous venv the shebang
    # in pipx still points to the old machines python location.
    python -m pip --disable-pip-version-check install --force-reinstall --no-deps "pipx==1.6.0" && RET=0 && break || RET=$? && sleep 1
    echo "Failed to install pipx, retrying..."
  done

  if [ $RET -ne 0 ]; then
    echo "Failed to install pipx"
    exit $RET
  fi

  for i in {1..5}; do
    pipx install --force "db-contrib-tool==0.8.5" --pip-args="--no-cache-dir" && RET=0 && break || RET=$? && sleep 1
    echo "Failed to install db-contrib-tool, retrying..."
  done

  if [ $RET -ne 0 ]; then
    echo "Failed to install db-contrib-tool"
    exit $RET
  fi
}
