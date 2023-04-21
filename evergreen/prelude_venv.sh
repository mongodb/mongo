function activate_venv {
  # check if virtualenv is set up
  if [ -d "${workdir}/venv" ]; then
    if [ "Windows_NT" = "$OS" ]; then
      # Need to quote the path on Windows to preserve the separator.
      . "${workdir}/venv/Scripts/activate" 2> /tmp/activate_error.log
    else
      . ${workdir}/venv/bin/activate 2> /tmp/activate_error.log
    fi
    if [ $? -ne 0 ]; then
      echo "Failed to activate virtualenv: $(cat /tmp/activate_error.log)"
      exit 1
    fi
    python=python
  else
    if [ -z "$python" ]; then
      echo "\$python is unset. This should never happen"
      exit 1
    fi
    python=${python}
  fi

  if [ "Windows_NT" = "$OS" ]; then
    export PYTHONPATH="$PYTHONPATH;$(cygpath -w ${workdir}/src)"
  elif [ "$(uname)" = "Darwin" ]; then
    #SERVER-75626 After activating the virtual environment under the mocos host. the PYTHONPATH setting
    #is incorrect, and the site-packages directory of the virtual environment cannot be found in the sys.path.
    python_version=$($python -c 'import sys; print(f"{sys.version_info.major}.{sys.version_info.minor}")')
    export PYTHONPATH="${workdir}/venv/lib/python${python_version}/site-packages:${PYTHONPATH}:${workdir}/src"
  else
    export PYTHONPATH="$PYTHONPATH:${workdir}/src"
  fi

  echo "python set to $(which $python)"
}
