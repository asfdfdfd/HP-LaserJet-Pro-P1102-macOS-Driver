#include <stdio.h>
#include "zjs_engine.h"

/* Build the reference converter: the vendored engine with a plain
 * stdin/stdout main, i.e. the equivalent of the upstream foo2zjs CLI. */
int main(int argc, char **argv)
{
    return zjs_main(argc, argv, stdin);
}
