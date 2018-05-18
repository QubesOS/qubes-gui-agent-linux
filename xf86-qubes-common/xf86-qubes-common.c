#include "include/xf86-qubes-common.h"

static DevPrivateKeyRec xf86_qubes_pixmap_private_key;

Bool xf86_qubes_pixmap_register_private(void) {
    return dixRegisterPrivateKey(&xf86_qubes_pixmap_private_key,
                                 PRIVATE_PIXMAP, 0);
}

void xf86_qubes_pixmap_set_private(PixmapPtr pixmap,
                                   struct xf86_qubes_pixmap *priv) {
    dixSetPrivate(&pixmap->devPrivates, &xf86_qubes_pixmap_private_key,
                  priv);
}

struct xf86_qubes_pixmap *xf86_qubes_pixmap_get_private(PixmapPtr pixmap) {
    return dixLookupPrivate(&pixmap->devPrivates,
                            &xf86_qubes_pixmap_private_key);
}
