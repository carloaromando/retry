{ lib
, llvmPackages
, meson
, ninja
, pkg-config
, gtest
, asio
, withAsio ? true
, runTests ? false
}:

llvmPackages.stdenv.mkDerivation {
  pname = "retry-cpp";
  version = "0.1.0";

  src = ./.;

  nativeBuildInputs = [
    meson
    ninja
    pkg-config
  ];

  buildInputs = [
    gtest
  ] ++ lib.optional withAsio asio;

  mesonFlags = [
    (lib.mesonEnable "asio" withAsio)
    (lib.mesonBool "tests" runTests)
  ];

  doCheck = runTests;

  mesonCheckFlags = [ "--print-errorlogs" ];

  meta = with lib; {
    description = "Retry combinators C++23 header-only library";
    license = licenses.bsd3;
    platforms = with platforms; linux ++ darwin;
    maintainers = [ ];
  };
}
