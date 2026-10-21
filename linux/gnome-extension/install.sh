#!/bin/sh
set -eu

extension_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
data_root=${XDG_DATA_HOME:-"${HOME:?HOME must name your user folder}/.local/share"}
case "$data_root" in
    /*) ;;
    *) printf '%s\n' 'XDG_DATA_HOME must be an absolute path.' >&2; exit 1 ;;
esac
destination="$data_root/gnome-shell/extensions/clickshade@local"
mkdir -p "$destination"
for name in metadata.json extension.js core.js; do
    install -m 0644 "$extension_dir/$name" "$destination/$name"
done
printf 'Installed extension files in %s\n' "$destination"
printf '%s\n' 'Enable it with: gnome-extensions enable clickshade@local'
printf '%s\n' 'If GNOME cannot find the new extension, log out and back in, then enable it.'
printf '%s\n' 'Open ClickShade and choose Retry. See README.md for supported capabilities and validation limits.'
