setenv DISPLAY ":0"
setenv _JAVA_AWT_WM_NONREPARENTING "1"
if ( -f /var/run/qubes-service/software-rendering )
  setenv GSK_RENDERER "cairo"
  setenv GDK_DEBUG "gl-disable vulkan-disable"
  setenv LIBGL_ALWAYS_SOFTWARE "1"
endif
