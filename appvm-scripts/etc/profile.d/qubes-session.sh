if [ -O /tmp/qubes-session-env -a -z "$QUBES_ENV_SOURCED" ]; then
    . /tmp/qubes-session-env
fi
