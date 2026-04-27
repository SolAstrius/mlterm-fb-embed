{
  description = "mlterm-fb-embed dev shell — autotools + freetype + zig cross toolchain.";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { nixpkgs, flake-utils, ... }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = import nixpkgs { inherit system; };
      in
      {
        devShells.default = pkgs.mkShell {
          # Build deps for the upstream + fb-embed branches:
          #   - autoconf/automake/libtool/m4 : regenerate configure
          #     after touching configure.in (we deliberately don't
          #     ship a regenerated configure on the branch — see
          #     commit ff99d0cb).
          #   - pkg-config : configure.in's PKG_CHECK_MODULES uses it.
          #   - zig : same C toolchain used elsewhere in the
          #     scalar-evolution build, gives us cross-compile for
          #     free when we ship per-platform native libs.
          #   - freetype + fontconfig : optional but kept here so
          #     anti-alias / fontconfig configure paths can be
          #     exercised without re-entering the shell.
          #   - iconv : encodefilter dep.
          #   - libpng + libjpeg : ImageMagick uses them; the
          #     fb-dumper PPM output pipes through `display`/`feh`/
          #     ImageMagick during dev. Convenient, not load-bearing.
          packages = with pkgs; [
            autoconf
            automake
            libtool
            m4
            pkg-config
            gnumake

            zig

            freetype
            fontconfig
            libiconv
            libpng
            libjpeg

            # For the dev convenience tools:
            imagemagick
            file
          ];

          shellHook = ''
            export MLTERM_SRC="$PWD"
            echo "mlterm-fb-embed devshell."
            echo
            echo "Quick build (Linux fb, no embed):"
            echo "  autoreconf -fi && ./configure --with-gui=fb \\"
            echo "    --disable-anti-alias --disable-fontconfig --disable-otl"
            echo "  make"
            echo
            echo "Quick build (embed mode + dumper):"
            echo "  autoreconf -fi && ./configure --with-gui=fb --enable-fb-embed \\"
            echo "    --disable-anti-alias --disable-fontconfig --disable-otl"
            echo "  make && ls tool/fb-dumper/mlterm-fb-dumper"
            echo
          '';
        };
      });
}
