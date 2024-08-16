#ifndef INPUT_LABELS
#define INPUT_LABELS

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <xserver-properties.h>

/* Aligned with linux/input.h.
   Note that there are holes in the ABS_ range, these are simply replaced with
   MISC here */
const char* abs_labels[] = {
    AXIS_LABEL_PROP_ABS_X,              /* 0x00 */
    AXIS_LABEL_PROP_ABS_Y,              /* 0x01 */
    AXIS_LABEL_PROP_ABS_Z,              /* 0x02 */
    AXIS_LABEL_PROP_ABS_RX,             /* 0x03 */
    AXIS_LABEL_PROP_ABS_RY,             /* 0x04 */
    AXIS_LABEL_PROP_ABS_RZ,             /* 0x05 */
    AXIS_LABEL_PROP_ABS_THROTTLE,       /* 0x06 */
    AXIS_LABEL_PROP_ABS_RUDDER,         /* 0x07 */
    AXIS_LABEL_PROP_ABS_WHEEL,          /* 0x08 */
    AXIS_LABEL_PROP_ABS_GAS,            /* 0x09 */
    AXIS_LABEL_PROP_ABS_BRAKE,          /* 0x0a */
};

const char* rel_labels[] = {
    AXIS_LABEL_PROP_REL_X,
    AXIS_LABEL_PROP_REL_Y,
    AXIS_LABEL_PROP_REL_Z,
    AXIS_LABEL_PROP_REL_RX,
    AXIS_LABEL_PROP_REL_RY,
    AXIS_LABEL_PROP_REL_RZ,
    AXIS_LABEL_PROP_REL_HWHEEL,
    AXIS_LABEL_PROP_REL_DIAL,
    AXIS_LABEL_PROP_REL_WHEEL,
    AXIS_LABEL_PROP_REL_MISC
};

const char* btn_labels[] = {
    BTN_LABEL_PROP_BTN_LEFT,
    BTN_LABEL_PROP_BTN_MIDDLE,
    BTN_LABEL_PROP_BTN_RIGHT,
    BTN_LABEL_PROP_BTN_WHEEL_UP,
    BTN_LABEL_PROP_BTN_WHEEL_DOWN,
    BTN_LABEL_PROP_BTN_HWHEEL_LEFT,
    BTN_LABEL_PROP_BTN_HWHEEL_RIGHT,
    BTN_LABEL_PROP_BTN_SIDE,
    BTN_LABEL_PROP_BTN_EXTRA,
    BTN_LABEL_PROP_BTN_FORWARD,
    BTN_LABEL_PROP_BTN_BACK,
};



#endif
