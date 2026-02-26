{ pkgs ? import <nixpkgs> { }
, lib ? pkgs.lib
, stdenv ? pkgs.stdenv
}:

stdenv.mkDerivation rec {
  pname = "rnp";
  version = "unstable";

  src = ./.;

  buildInputs = with pkgs; [ zlib bzip2 json_c botan2 ];

  cmakeFlags = [
    "-DCMAKE_INSTALL_PREFIX=${placeholder "out"}"
    "-DBUILD_SHARED_LIBS=on"
    "-DBUILD_TESTING=on"
    "-DDOWNLOAD_GTEST=off"
  ];

  nativeBuildInputs = with pkgs; [ asciidoctor cmake gnupg gtest pkg-config python3 ];

  # NOTE: check-only inputs should ideally be moved to checkInputs, but it
  # would fail during buildPhase.
  # checkInputs = [ gtest python3 ];

  outputs = [ "out" "lib" "dev" ];

  preConfigure = ''
    commitEpoch=$(date +%s)
    baseVersion=$(cat version.txt)
    echo "v$baseVersion-0-g0-dirty+$commitEpoch" > version.txt
    # For generating the correct timestamp in cmake
    export SOURCE_DATE_EPOCH=$commitEpoch
  '';

  meta = with lib; {
    homepage = "https://github.com/rnpgp/rnp";
    description = "High performance C++ OpenPGP library, fully compliant to RFC 4880";
    license = licenses.bsd2;
    platforms = platforms.all;
    maintainers = with maintainers; [ ribose-jeffreylau ];
  };
}