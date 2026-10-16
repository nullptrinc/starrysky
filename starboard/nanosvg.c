/* SPDX-License-Identifier: Zlib */
/* Compiles the vendored NanoSVG parser and rasterizer (nanosvg/, unmodified). */
#include <math.h>
#include <stdio.h>
#include <string.h>
#define NANOSVG_IMPLEMENTATION
#include "nanosvg/nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvg/nanosvgrast.h"
