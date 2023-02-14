#ifndef XF86_QUBES_COMMON_H
#define XF86_QUBES_COMMON_H

// Order is important!
#include <xorg-server.h>
#include <xf86.h>

// Metadata of mapped grant pages
struct xf86_qubes_pixmap {
    size_t pages; // Number of pages
    uint32_t *refs; // Pointer to grant references
    uint8_t *data; // Local mapping
    uint32_t refcount; // 1-biased reference count: stores number of references
                       // minus 1
};

// Only intended for use in the Qubes xorg modules.

extern _X_EXPORT Bool xf86_qubes_pixmap_register_private(void);

extern _X_EXPORT void xf86_qubes_pixmap_set_private(
        PixmapPtr pixmap,
        struct xf86_qubes_pixmap *priv);

extern _X_EXPORT struct xf86_qubes_pixmap *xf86_qubes_pixmap_get_private(
        PixmapPtr pixmap);

_X_EXPORT void xf86_qubes_pixmap_incref(struct xf86_qubes_pixmap *);
_X_EXPORT void xf86_qubes_free_pixmap_private(struct xf86_qubes_pixmap *);
_X_EXPORT void xf86_qubes_pixmap_add_to_list(struct xf86_qubes_pixmap *);
_X_EXPORT void xf86_qubes_pixmap_remove_list_head(void);
_X_EXPORT void xf86_qubes_pixmap_remove_list_all(void);

// xenctrl and xorg headeres are not compatible, so define the required
// constants here.
#ifndef XC_PAGE_SHIFT
#define XC_PAGE_SHIFT           12
#define XC_PAGE_SIZE            (1UL << XC_PAGE_SHIFT)
#define XC_PAGE_MASK            (~(XC_PAGE_SIZE-1))
#endif

#endif
