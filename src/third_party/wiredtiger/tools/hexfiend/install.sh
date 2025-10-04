#!/bin/bash

SRCDIR="$(dirname -- ${BASH_SOURCE[0]:-$0})"
DSTDIR="$HOME/Library/Application Support/com.ridiculousfish.HexFiend"

if [[ -d "/Applications/Hex Fiend.app" ]]; then
  echo "=== HexFiend already installed"
else
  echo "=== Installing HexFiend ..."
  brew install --cask hex-fiend
fi


echo
echo "=== Copying templates ..."
mkdir -p "$DSTDIR" && cp -a "$SRCDIR/Templates" "$DSTDIR/"


cat << _END

=== If you want a 'hexparse' binary in your path, do either of these:

    $ cp "$HOME/Library/Application Support/com.ridiculousfish.HexFiend/Templates/hexparse" /usr/local/bin/
or
    $ ln -s "$HOME/Library/Application Support/com.ridiculousfish.HexFiend/Templates/hexparse" /usr/local/bin/
or add this to your .bashrc:
    alias hexparse="$HOME/Library/Application Support/com.ridiculousfish.HexFiend/Templates/hexparse"

_END

