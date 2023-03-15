#!/bin/sh

CSS=$(cat << _EOF
* {
    /* -----------------------------------------------------------
     https://github.com/QubesOS/qubes-issues/issues/8081

     As a workaround for the above issues, disable box-shadow in
     GTK4 applications.
    ------------------------------------------------------------ */
    box-shadow: none;
}
_EOF
)

if [ ! -e ~/.config/gtk-4.0/qubes-box-shadow-disabled ]; then
    mkdir -p ~/.config/gtk-4.0
    echo "${CSS}" >> ~/.config/gtk-4.0/gtk.css
    touch ~/.config/gtk-4.0/qubes-box-shadow-disabled
fi
