#!/usr/bin/env bash
# Link the DWARF out of the build's .o files into a self-contained .dSYM bundle.
#
# A RelWithDebInfo link does not embed debug info; it embeds a "debug map" of
# references to each .o file *and its mtime*. LLDB refuses any .o whose mtime has
# moved since the link, so a rebuild silently blinds the debugger against an
# already-running game -- pool walks then return zero rows, which reads as an
# empty game rather than as a failure. A .dSYM has no such dependency: once it
# exists, LLDB reads types from it and never consults a .o again.
#
# Bundles are archived by binary UUID so a rebuild mid-session cannot orphan the
# running game's symbols; the sibling openttd.dSYM (what LLDB finds on its own)
# points at the current one. Set OPENTTD_SKIP_DSYM=1 to skip.
set -euo pipefail

[[ "${OPENTTD_SKIP_DSYM:-0}" == "1" ]] && exit 0

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="$repo_root/build"
binary="$build_dir/openttd"

if [[ ! -x "$binary" ]]; then
  echo "make_dsym: no binary at $binary" >&2
  exit 1
fi

uuid="$(dwarfdump --uuid "$binary" | awk '{print $2; exit}')"
if [[ -z "$uuid" ]]; then
  echo "make_dsym: could not read UUID from $binary" >&2
  exit 1
fi

archive="$build_dir/dsyms/$uuid.dSYM"
if [[ ! -d "$archive" ]]; then
  mkdir -p "$build_dir/dsyms"
  # dsymutil warns about .o files a later build has already removed; those
  # translation units simply keep the debug map's fate and are not fatal here.
  dsymutil "$binary" -o "$archive"
fi

# What LLDB finds without being told anything.
ln -sfn "$archive" "$binary.dSYM"

# Retain older archives: a game can stay running across any number of rebuilds.
# Remove unneeded bundles manually once their games and debugger sessions close.

echo "make_dsym: symbols at $archive"
