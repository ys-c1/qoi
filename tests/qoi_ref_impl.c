/* Reference implementation: pristine qoi.h from git HEAD with renamed
symbols, compiled as its own translation unit so it can be linked alongside
the refactored qoi.h for byte-exact differential testing. */

#define qoi_encode qoi_ref_encode
#define qoi_decode qoi_ref_decode
#define qoi_write  qoi_ref_write
#define qoi_read   qoi_ref_read

#define QOI_IMPLEMENTATION
#include "qoi_ref.h"
