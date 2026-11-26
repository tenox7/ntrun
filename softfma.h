/*
 * softfma.h: a correctly rounded fused multiply-add for hosts whose C library has no exact one.
 * The fma of msvcrt.dll (MINGW64 of MSYS2) does not round once: the PowerPC multiply-add
 * instructions give other bits than the CPU. soft_fma(a, b, c) is a * b + c rounded once.
 *
 * Method (Boldo and Melquiond, "Emulation of FMA and correctly rounded sums"): a * b is the sum
 * of two doubles (p, e) with no error (the product with Dekker's split); c + p is the sum of two
 * doubles (th, tl) with no error; tl + e is rounded to odd, and th + that is rounded to
 * nearest: one rounding of the exact sum. A result that is too large, too small or not finite
 * is done in long double, which is the best that this does there.
 */
#ifndef SOFTFMA_H
#define SOFTFMA_H

#include <math.h>
#include <stdint.h>
#include <string.h>

static double soft_fma(double a, double b, double c)
{
    double p = a * b, ah, al, bh, bl, e, th, tl, z, vh, vl, s;
    uint64_t bits;

    if (!isfinite(p) || !isfinite(c) || !isfinite(a) || !isfinite(b) || fabs(p) > 1e290 || fabs(p) < 1e-280 ||
            fabs(a) > 1e290 || fabs(b) > 1e290 || fabs(a) < 1e-280 || fabs(b) < 1e-280)
        return (double)((long double)a * b + c);

    /* a * b = p + e, with no error */
    s = 134217729.0 * a; /* 2^27 + 1 */
    ah = s - (s - a);
    al = a - ah;
    s = 134217729.0 * b;
    bh = s - (s - b);
    bl = b - bh;
    e = ((ah * bh - p) + ah * bl + al * bh) + al * bl;

    /* c + p = th + tl, with no error */
    th = c + p;
    z = th - c;
    tl = (c - (th - z)) + (p - z);

    /* tl + e rounded to odd: if the sum is not exact, the last bit of the result is 1 */
    vh = tl + e;
    z = vh - tl;
    vl = (tl - (vh - z)) + (e - z);
    if (vl != 0.0) {
        memcpy(&bits, &vh, sizeof bits);
        if (!(bits & 1))
            vh = nextafter(vh, vl > 0.0 ? INFINITY : -INFINITY);
    }
    return th + vh;
}

#endif
