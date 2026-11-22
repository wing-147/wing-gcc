// Compile-time language-mode switches. There is deliberately no generated
// configuration layer: the current front end has one independent switch, which
// callers may override from the command line:
//
//   make CFLAGS="-Wall -g -Iinclude -DGNU_EXTENSION=1"
//
// GNU_EXTENSION=0 is the default C11-facing mode. Setting it to 1 enables only
// the GNU lexical and grammar extensions currently implemented; it does not
// claim complete GNU C compatibility.
#ifndef WING_CONFIG_H
#define WING_CONFIG_H

#ifndef GNU_EXTENSION
#define GNU_EXTENSION 0
#endif

#endif // WING_CONFIG_H
