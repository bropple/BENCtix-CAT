#!/bin/bash
# wireplumber without its documentation or GObject introspection: both need
# doxygen (and the docs graphviz and sphinx), which are slow under emulation
# and pull large stacks. Nothing here uses the introspection bindings.
#   wireplumber.sh PKGBUILD
set -euo pipefail
f=$1
sed -i -E \
	-e '/^\s*wireplumber-docs$/d' \
	-e '/^\s*(doxygen|graphviz|gobject-introspection|python-breathe|python-lxml|python-sphinx|python-sphinx_rtd_theme)$/d' \
	-e 's/^(\s*)-D system-lua=true$/\1-D system-lua=true\n\1-D doc=disabled\n\1-D introspection=disabled/' \
	-e '/_pick docs usr\/share\/doc\/wireplumber\/html/d' \
	-e '/_pick libw usr\/lib\/girepository-1.0/d' \
	-e '/_pick libw usr\/share\/gir-1.0/d' \
	"$f"
grep -q -- '-D doc=disabled' "$f" || { echo "wireplumber.sh: doc=disabled not added" >&2; exit 1; }
grep -q -- '-D introspection=disabled' "$f" || { echo "wireplumber.sh: introspection=disabled not added" >&2; exit 1; }
! grep -qE '^\s*(wireplumber-docs|doxygen|graphviz|gobject-introspection)$' "$f" || { echo "wireplumber.sh: docs or introspection still built" >&2; exit 1; }
! grep -qE '_pick (docs|libw usr/(lib/girepository|share/gir))' "$f" || { echo "wireplumber.sh: docs or GIR still picked" >&2; exit 1; }
