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

  timeout_and_retry 180 python -m pipx install -vv --force "db-contrib-tool==0.8.5" --pip-args="--no-cache-dir"
}
