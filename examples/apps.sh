#!/usr/bin/env bash
# App launcher: lists the installed desktop applications (.desktop files) and
# runs the picked one detached, like a desktop's app menu.
#   examples/apps.sh
# Set LM10 if lm10 isn't on PATH, e.g. LM10=./bld/debug/lm10 examples/apps.sh
set -euo pipefail

# Prints the lm10 line for one .desktop file, or nothing if it shouldn't be
# listed. Exec is already quoted the way lm10 lines are, minus its %f-style
# field codes.
entry() {
  awk '
    function q(s) { gsub(/\047/, "\047\\\047\047", s); return "\047" s "\047" }
    /^\[/ { in_entry = $0 == "[Desktop Entry]"; next }
    !in_entry || !(i = index($0, "=")) { next }
    {
      k = substr($0, 1, i - 1); sub(/[ \t]+$/, "", k)
      v = substr($0, i + 1); sub(/^[ \t]+/, "", v)
      if (!(k in f)) f[k] = v
    }
    END {
      if (f["Type"] != "Application" || f["Exec"] == "" ||
          f["NoDisplay"] == "true" || f["Hidden"] == "true" ||
          f["Terminal"] == "true")
        exit
      exec = f["Exec"]
      gsub(/%[fFuUdDnNickvm]/, "", exec)
      gsub(/%%/, "%", exec)
      sub(/[ \t]+$/, "", exec)
      printf "-X -n %s -i %s -- %s\n", q(f["Name"]), q(f["Icon"]), exec
    }' "$1"
}

# The first file with a given desktop id wins, so ~/.local/share overrides
# (or, with Hidden=true, removes) the system's entries.
declare -A seen
IFS=: read -ra dirs <<<"${XDG_DATA_HOME:-$HOME/.local/share}:${XDG_DATA_DIRS:-/usr/local/share:/usr/share}"
for dir in "${dirs[@]}"; do
  [[ -d $dir/applications ]] || continue
  while IFS= read -r -d '' file; do
    id=${file#"$dir/applications/"}
    id=${id//\//-}
    [[ -z ${seen[$id]+x} ]] || continue
    seen[$id]=1
    entry "$file"
  done < <(find -L "$dir/applications" -name '*.desktop' -print0)
done | sort -f | "${LM10:-lm10}"
