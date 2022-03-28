function setup_db_contrib_tool_venv {
  local db_contrib_tool_venv_dir="${workdir}/db_contrib_tool_venv"
  if [ -d "$db_contrib_tool_venv_dir" ]; then
    echo "Found existing db-contrib-tool venv. Skipping setup."
    exit 0
  fi

  $python -m venv "$db_contrib_tool_venv_dir"

  if [ "Windows_NT" = "$OS" ]; then
    dos2unix "$db_contrib_tool_venv_dir/Scripts/activate"
  fi

  activate_db_contrib_tool_venv
  python -m pip --disable-pip-version-check install "pip==21.0.1" "wheel==0.37.0" || exit 1
  python -m pip --disable-pip-version-check install "db-contrib-tool==0.2.1" || exit 1
}

function activate_db_contrib_tool_venv {
  local db_contrib_tool_venv_dir="${workdir}/db_contrib_tool_venv"
  if [ ! -d "$db_contrib_tool_venv_dir" ]; then
    echo "Could not find db-contrib-tool venv."
    exit 1
  fi

  if [ "Windows_NT" = "$OS" ]; then
    . "$db_contrib_tool_venv_dir/Scripts/activate"
  else
    . "$db_contrib_tool_venv_dir/bin/activate"
  fi
}
