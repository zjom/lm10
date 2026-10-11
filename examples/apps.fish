#!/usr/bin/env fish
# App launcher: lists the installed desktop applications (.desktop files) and
# runs the picked one detached, like a desktop's app menu.
#   examples/apps.fish
# Set LM10 if lm10 isn't on PATH, e.g. LM10=./bld/debug/lm10 examples/apps.fish
# Needs fd.

set -q XDG_DATA_HOME; or set XDG_DATA_HOME ~/.local/share
set -q XDG_DATA_DIRS; or set XDG_DATA_DIRS /usr/local/share:/usr/share
set -q LM10; or set LM10 lm10

# The first value of a key in the given lines.
function field -a key
    set -l v (string replace -rf "^$key\s*=\s*" '' -- $argv[2..])
    echo $v[1]
end

# The first file with a given desktop id wins, so ~/.local/share overrides
# (or, with Hidden=true, removes) the system's entries.
set -l seen
for dir in (string split : -- $XDG_DATA_HOME:$XDG_DATA_DIRS)/applications
    test -d $dir; or continue
    for file in (fd -L -u -e desktop --base-directory $dir)
        set -l id (string replace -a / - -- $file)
        contains -- $id $seen; and continue
        set -a seen $id

        set -l e (sed -n '/^\[Desktop Entry\]/,/^\[/p' $dir/$file)
        set -l exec (field Exec $e | string replace -ar '%[fFuUdDnNickvm]' '' | string replace -a %% % | string trim -r)
        test (field Type $e) = Application -a -n "$exec"; or continue
        contains true (field NoDisplay $e) (field Hidden $e) (field Terminal $e); and continue

        # Exec is already quoted the way lm10 lines are.
        set -l name (string replace -a "'" "'\\''" -- (field Name $e))
        echo "-X -n '$name' -- $exec"
    end
end | sort -f | $LM10
