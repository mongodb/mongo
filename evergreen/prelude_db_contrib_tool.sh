function setup_db_contrib_tool {
  # check if db-contrib-tool is already installed
  if [[ $(type -P db-contrib-tool) ]]; then
    return 0
  fi

  $python evergreen/download_db_contrib_tool.py
}

function use_db_contrib_tool_mongot {
  # Checking that this is not a downstream patch on mongod created by mongot's patch trigger.
  # In the case that it's not, download latest (eg HEAD of 10gen/mongot) or the
  # release (eg currently running in production on Atlas) mongot binary.
  arch=$(uname -i)
  if [[ ! $(declare -p linux_x86_64_mongot_localdev_binary linux_aarch64_mongot_localdev_binary macos_x86_64_mongot_localdev_binary 2> /dev/null) ]]; then

    if [ "${download_mongot_release}" = "true" ]; then
      mongot_version="release"
    else
      mongot_version="latest"
    fi

    if [[ "$OSTYPE" == "linux-gnu"* ]]; then
      mongot_platform="linux"
    elif [[ "$OSTYPE" == "darwin"* ]]; then
      mongot_platform="macos"
    else
      echo "mongot is only supported on linux and mac and does not support ${OSTYPE}"
      exit 1
    fi

    mongot_arch="x86_64"
    # macos arm64 is not supported by mongot, but macos x86_64 runs on it successfully
    if [[ $arch == "aarch64"* ]] && [[ "$OSTYPE" != "darwin"* ]]; then
      mongot_arch="aarch64"
    fi
    echo "running: db-contrib-tool setup-mongot-repro-env ${mongot_version} --platform=${mongot_platform} --architecture=${mongot_arch} --installDir=."
    # This should create the folder mongot-localdev, usually run at the root of mongo directory
    db-contrib-tool setup-mongot-repro-env ${mongot_version} --platform=${mongot_platform} --architecture=${mongot_arch} --installDir=.
  else
    # This is a downstream patch, which means there is a patched mongot binary we need to install.
    if [[ "$OSTYPE" == "linux-gnu"* ]]; then
      if [[ $arch == "x86_64"* ]]; then
        mongot_url=${linux_x86_64_mongot_localdev_binary}
      elif [[ $arch == "aarch64"* ]]; then
        mongot_url=${linux_aarch64_mongot_localdev_binary}
      else
        echo "mongot-localdev does not support ${arch}"
        exit 1
      fi
    elif [[ "$OSTYPE" == "darwin"* ]]; then
      mongot_url=${macos_x86_64_mongot_localdev_binary}
    else
      echo "mongot-localdev does not support ${OSTYPE}"
      exit 1
    fi
    echo "running curl ${mongot_url} | tar xvz"
    # This should create the folder mongot-localdev, usually run at the root of mongo directory
    curl ${mongot_url} | tar xvz
  fi
  # Hack to remove BUILD.bazel file that can be lying around in mongot
  rm -f ./mongot-localdev/bin/jdk/BUILD.bazel
}
