/* zjs_engine.h - ZjStream encoder for HP LaserJet Pro P1102.
 *
 * Vendored from foo2zjs (GPL-2.0), patched: main() -> zjs_main() with
 * an explicit input FILE*, so it can be driven in-process by a CUPS filter.
 */
#ifndef ZJS_ENGINE_H
#define ZJS_ENGINE_H

#include <stdio.h>

/* Parse options (getopt-style argv), prepare globals, then read a
 * multi-page P4 (pbmraw) stream from "in" and write the complete
 * ZjStream document (PJL + ZJS chunks) to stdout.  Exits the process
 * on fatal errors, mirroring upstream foo2zjs behavior. */
int zjs_main(int argc, char *argv[], FILE *in);

#endif /* ZJS_ENGINE_H */
