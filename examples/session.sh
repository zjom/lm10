#!/bin/sh
# Session menu: lock, log out, suspend and so on.
#   examples/session.sh
# Set LM10 if lm10 isn't on PATH.
#
# -X runs the picked command detached. Commands aren't run by a shell, so
# Log out uses sh -c for its variable.
exec "${LM10:-lm10}" <<'ENTRIES'
-n Lock -X loginctl lock-session
-n 'Log out' -X sh -c 'loginctl terminate-session "$XDG_SESSION_ID"'
-n Suspend -X systemctl suspend
-n Hibernate -X systemctl hibernate
-n Reboot -X systemctl reboot
-n 'Reboot into firmware setup' -X systemctl reboot --firmware-setup
-n 'Power off' -X systemctl poweroff
ENTRIES
