#!/bin/sh
# Session menu: lock, log out, suspend and so on.
#   examples/session.sh
# Set LM10 if lm10 isn't on PATH.
#
# -X runs the picked command detached. Commands aren't run by a shell, so
# Log out uses sh -c for its variable.
exec "${LM10:-lm10}" <<'ENTRIES'
-n Lock -i system-lock-screen -X loginctl lock-session
-n 'Log out' -i system-log-out -X sh -c 'loginctl terminate-session "$XDG_SESSION_ID"'
-n Suspend -i system-suspend -X systemctl suspend
-n Hibernate -i system-hibernate -X systemctl hibernate
-n Reboot -i system-reboot -X systemctl reboot
-n 'Reboot into firmware setup' -i system-reboot -X systemctl reboot --firmware-setup
-n 'Power off' -i system-shutdown -X systemctl poweroff
ENTRIES
