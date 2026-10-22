/* Hand-written replacements for lifted NFSC.exe functions, by guest VA.
 * Carbon's addresses are discovered on its own binary; none is inherited
 * from the Underground or Most Wanted ports (Most Wanted replaced its static
 * CRT memmove; Carbon's memmove lives in msvcr71.dll). */
#include "nfs_runtime.h"
#include "recomp_types.h"
#include <math.h>

/* msvcr71 _CIpow: ST(1) is the base, ST(0) the exponent. Its handwritten
 * x87 classification/error paths are not faithfully represented by the
 * lifter's double stack. Use libm for the operation, not an SEH bypass.
 * Consume two x87 operands, produce one, and leave older stack slots intact.
 * The intrinsic has no arguments on the integer stack (plain ret).
 */
static void carbon_ci_pow(void) {
    double exponent = fp_pop_impl(g_st, &g_fp_top);
    double base = fp_pop_impl(g_st, &g_fp_top);
    fp_push_impl(g_st, &g_fp_top, pow(base, exponent));
    g_esp += 4;
}
nfs_override_fn nfs_override_lookup(uint32_t va) {
    return va == 0x00f6406fu ? carbon_ci_pow : 0;
}
int nfs_override_count(void) { return 1; }
