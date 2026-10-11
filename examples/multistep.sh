#!/usr/bin/env bash
# Process killer in two steps: pick one of your processes, then the signal to
# send it. Cancelling the second list goes back to the first.
#   examples/multistep.sh
# Set LM10 if lm10 isn't on PATH.
#
# The first list's entries have neither -x nor -X, so picking one prints its
# command line (here just the pid) for the script to read. The second list's
# entries use -x, so lm10 execs kill in its place and its status is kill's.
# Exit status 130 means a list was cancelled.
set -uo pipefail

lm10=${LM10:-lm10}

while :; do
  pid=$(
    ps -u "$USER" -o pid=,comm= --sort=-start_time |
      while read -r pid comm; do
        ((pid == $$)) && continue
        printf -- '-n %q -- %s\n' "$comm ($pid)" "$pid"
      done |
      "$lm10" --prompt 'process ❯ '
  ) || exit

  comm=$(ps -p "$pid" -o comm=) || {
    echo "process $pid is gone" >&2
    continue
  }

  "$lm10" --prompt 'signal ❯ ' --header "$comm ($pid)  ·  Escape goes back" <<ENTRIES
-n 'TERM: ask it to quit' -x kill -s TERM -- $pid
-n 'INT: as Control+c' -x kill -s INT -- $pid
-n 'HUP: hang up, often reloads' -x kill -s HUP -- $pid
-n 'STOP: pause' -x kill -s STOP -- $pid
-n 'CONT: resume' -x kill -s CONT -- $pid
-n 'KILL: end it now' -x kill -s KILL -- $pid
ENTRIES
  status=$?
  ((status == 130)) || exit "$status"
done
