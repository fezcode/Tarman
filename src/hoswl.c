/* The hoswl client implementation lives in its own translation unit because it
 * includes <windows.h>, which must never meet raylib.h (see sys.h). */
#define HOSWL_IMPLEMENTATION
#include "hoswl.h"

