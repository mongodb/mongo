function activate_venv {
    # check if virtualenv is set up
    if [ -d "${workdir}/venv" ]; then
        # It's possible for activate to fail without stderr; as a result the cat operation (below) could fail.
        # To mitigate this, create an empty error log.
        # We're relying on the evergreen provided tmp directory here because Amazon Linux 2023 has an issue
        # writing to /tmp/ on startup.
        touch $TMPDIR/activate_error.log
        if [ "Windows_NT" = "$OS" ]; then
            # Need to quote the path on Windows to preserve the separator.
            . "${workdir}/venv/Scripts/activate" 2>$TMPDIR/activate_error.log
        else
            . ${workdir}/venv/bin/activate 2>$TMPDIR/activate_error.log
        fi
        if [ $? -ne 0 ]; then
            echo "Failed to activate virtualenv: $(cat $TMPDIR/activate_error.log)"
            exit 1
        fi
        python=python
    else
        if [ -z "$python" ]; then
            echo "\$python is unset. This should never happen"
            exit 1
        fi
        python=${python}
        echo "Could not find venv. Setting python to $python."
    fi

    if [ "Windows_NT" = "$OS" ]; then
        export PYTHONPATH="$PYTHONPATH;$(cygpath -w ${workdir}/src)"
    elif [ "$(uname)" = "Darwin" ]; then
        #SERVER-75626 After activating the virtual environment under the mocos host. the PYTHONPATH setting
        #is incorrect, and the site-packages directory of the virtual environment cannot be found in the sys.path.
        python_version=$($python -c 'import sys; print(f"{sys.version_info.major}.{sys.version_info.minor}")')
        export PYTHONPATH="${workdir}/venv/lib/python${python_version}/site-packages:${PYTHONPATH}:${workdir}/src"
    else
        python_version=$($python -c 'import sys; print(f"{sys.version_info.major}.{sys.version_info.minor}")')
        site_packages="${workdir}/venv/lib/python${python_version}/site-packages"
        python -c "import sys; print(sys.path)"

        # Check if site_packages is already in sys.path
        in_sys_path=$($python -c "import sys; print('$site_packages' in sys.path)")
        if [ "$in_sys_path" = "False" ]; then
            export PYTHONPATH="${site_packages}:${PYTHONPATH}:${workdir}/src"
        else
            export PYTHONPATH="$PYTHONPATH:${workdir}/src"
        fi
        python -c "import sys; print(sys.path)"
    fi

    echo "python set to $(which $python) and python version: $($python --version)"
}
