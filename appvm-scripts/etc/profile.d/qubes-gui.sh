export DISPLAY=:0 _JAVA_AWT_WM_NONREPARENTING=1
if test -f /var/run/qubes-service/software-rendering; then
  export GSK_RENDERER="cairo" GDK_DEBUG="gl-disable vulkan-disable" \
         LIBGL_ALWAYS_SOFTWARE=1
fi
