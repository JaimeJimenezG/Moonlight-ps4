#!/bin/bash
# Translate clang-style linker invocations to OpenOrbis ld.lld.
OO="${OO_PS4_TOOLCHAIN:-$HOME/ps4dev/OpenOrbis/PS4Toolchain}"
args=()
skip=
for a in "$@"; do
  case "$a" in
    -fuse-ld=*|--target=*|-pie) ;; # ignore
    -Wl,*)
      # expand -Wl,foo,bar
      IFS=',' read -ra parts <<< "${a#-Wl,}"
      for p in "${parts[@]}"; do args+=("$p"); done
      ;;
    *) args+=("$a") ;;
  esac
done
exec ld.lld -m elf_x86_64 --eh-frame-hdr --script "$OO/link.x" "$OO/lib/crt1.o" -L"$OO/lib" "${args[@]}" -lc -lkernel
