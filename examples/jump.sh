# Directory jumper: source this from bash or zsh, then run `j` to pick one of
# the git repositories under ~/projects (or the directories given) and cd
# into it.
#   . examples/jump.sh
#   j               # repos under ~/projects
#   j ~/src /etc    # repos under these instead
# Set LM10 if lm10 isn't on PATH. Needs jq and GNU find.
#
# Entries without -x/-X print their command line, shell-quoted, when picked.
# That lets the calling shell eval it, which is the only way to run something
# like cd that has to happen in that shell. jq builds the entries, quoting
# the paths.
j() {
  local cmd
  cmd=$(
    find "${@:-$HOME/projects}" -mindepth 1 -maxdepth 4 \
      -name .git -prune -printf '%h\n' 2>/dev/null |
      sort -u |
      jq -Rr --arg home "$HOME" '
        (if startswith($home + "/") then "~" + .[($home | length):]
         else . end) as $name |
        "-n \($name | @sh) -i folder cd -- \(. | @sh)"' |
      "${LM10:-lm10}"
  ) && [ -n "$cmd" ] && eval "$cmd"
}
