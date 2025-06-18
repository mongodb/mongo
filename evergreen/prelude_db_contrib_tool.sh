function setup_db_contrib_tool {
  # check if db-contrib-tool is already installed
  if [[ $(type -P db-contrib-tool) ]]; then
    return 0
  fi

  $python evergreen/download_db_contrib_tool.py
}
