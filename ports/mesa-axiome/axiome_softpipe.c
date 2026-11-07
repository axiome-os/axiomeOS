#include "axiome_winsys.h"

/* Glue: create a softpipe screen and wrap the axiome winsys.
   softpipe provides all rendering; axiome winsys provides display-target
   resource allocation and axdri_present() frontbuffer presentation. */

struct pipe_screen *axiome_softpipe_create_screen(int fd)
{
    /* The real Mesa build links libsoftpipe. This file is a placeholder;
       the full integration wires softpipe_screen_create() to use our
       resource_create/present hooks via the winsys interface table.
       See WINSYS.md for the meson.build patch series. */
    (void)fd;
    return NULL;
}
