if ! ([ -n "$SSH_CLIENT" ] || [ -n "$SSH_TTY" ] ]); then
  export DISPLAY=:0 _JAVA_AWT_WM_NONREPARENTING=1
fi
if test -f /var/run/qubes-service/software-rendering; then
  export GSK_RENDERER="cairo" \
         GDK_DISABLE="gl vulkan" \
         GDK_DEBUG="gl-disable vulkan-disable" \
         LIBGL_ALWAYS_SOFTWARE=1
fi
