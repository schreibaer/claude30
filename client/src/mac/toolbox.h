/* Toolbox headers. Retro68's Multiversal interfaces have no separate
   Controls.h, Scrap.h or Folders.h (everything is in Multiverse.h); Apple's
   Universal Interfaces do. */
#ifndef TOOLBOX_H
#define TOOLBOX_H

#if defined(__has_include)
#  if __has_include(<Multiverse.h>)
#    include <Multiverse.h>
#    define TOOLBOX_INCLUDED 1
#  endif
#endif
#ifndef TOOLBOX_INCLUDED
#  include <Controls.h>
#  include <Folders.h>
#  include <Scrap.h>
#endif

#endif
