#!/usr/bin/env bash
# tmux session picker, after sesh's fzf binding: picking a session attaches
# to it (or switches to it from inside tmux).
#   examples/tmux.sh
#   Control+a  tmux sessions (the start)
#   Control+x  zoxide directories; picking one makes a session there
#   Control+d  kill the selected session
# Set LM10 if lm10 isn't on PATH. Control+x needs zoxide.
#
# The lists are made by this script itself (tmux.sh --list WHAT), so the
# reload actions can run them too.
set -euo pipefail

if [[ -n ${TMUX:-} ]]; then
  attach=(switch-client -t)
else
  attach=(attach-session -t)
fi

# Prints entries as lines, quoted with %q, which lm10 reads back.
list() {
  case $1 in
  sessions)
    tmux list-sessions -F '#{session_name}' 2>/dev/null |
      while IFS= read -r s; do
        printf -- '-x -i utilities-terminal -n %q -- tmux %s %q\n' \
          "$s" "${attach[*]}" "$s"
      done
    ;;
  zoxide)
    zoxide query -l | while IFS= read -r dir; do
      name=$(basename "$dir" | tr '.:' '__')
      printf -- '-x -i folder -n %q -- tmux new-session -A -s %q -c %q\n' \
        "$dir" "$name" "$dir"
    done
    ;;
  esac
}

if [[ ${1:-} == --list ]]; then
  list "$2"
  exit
fi

printf -v self %q "$(realpath "$0")"
# The session is the last argv word: tmux ATTACH -t SESSION is {3}. The
# prompt shows which list is shown; the header lists the keys.
list sessions | "${LM10:-lm10}" \
  --prompt '⚡ ' \
  --header '^a sessions  ·  ^x zoxide  ·  ^d kill session' \
  --bind "Control+a=change-prompt(⚡ )+reload($self --list sessions)" \
  --bind "Control+x=change-prompt(📁 )+reload($self --list zoxide)" \
  --bind "Control+d=execute(tmux kill-session -t {3})+change-prompt(⚡ )+reload($self --list sessions)"
