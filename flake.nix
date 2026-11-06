{
  description = "Retry combinators C++23 header-only library";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-24.11";

    flake-parts = {
      url = "github:hercules-ci/flake-parts";
      inputs.nixpkgs-lib.follows = "nixpkgs";
    };
  };

  outputs = inputs@{ flake-parts, ... }:
    flake-parts.lib.mkFlake { inherit inputs; } {
      systems = [ "x86_64-linux" "aarch64-linux" "aarch64-darwin" "x86_64-darwin" ];

      perSystem = { pkgs, ... }:
        let
          llvmPackages = pkgs.llvmPackages_19;

          # nixpkgs 24.11 ships Asio 1.24, whose clang coroutine detection
          # still looks for the Coroutines TS (<experimental/coroutine>).
          # Clang removed that in 17, so asio::awaitable would not exist.
          #
          # Asio is header-only, so we skip its autotools build entirely and
          # just drop the headers plus a pkg-config file into the store.
          asio = let version = "1.34.2"; in
            pkgs.runCommand "asio-${version}"
              {
                src = pkgs.fetchurl {
                  url = "mirror://sourceforge/asio/asio-${version}.tar.bz2";
                  hash = "sha256-nL5eir783jyycFZyIQVIo+noKxNoKj0oKLw00/4bVYM=";
                };

                meta = {
                  description = "Cross-platform C++ library for network and low-level I/O";
                  homepage = "https://think-async.com/Asio/";
                  license = pkgs.lib.licenses.boost;
                };
              }
              ''
                tar xf $src

                mkdir -p $out/include $out/lib/pkgconfig
                cp -r asio-${version}/include/asio.hpp asio-${version}/include/asio $out/include/

                printf '%s\n' \
                  "prefix=$out" \
                  'includedir=''${prefix}/include' \
                  "" \
                  'Name: asio' \
                  'Description: Asio C++ library (header only)' \
                  'Version: ${version}' \
                  'Cflags: -I''${includedir}' \
                  > $out/lib/pkgconfig/asio.pc
              '';

          retry = pkgs.callPackage ./default.nix {
            inherit llvmPackages asio;
          };
        in
        {
          packages.default = retry;
          packages.unittests = retry.override { runTests = true; };
          packages.no-asio = retry.override { withAsio = false; };

          devShells.default = pkgs.mkShell {
            packages = [
              llvmPackages.clang
              llvmPackages.clang-tools
            ];

            inputsFrom = [ retry ];
          };
        };
    };
}
