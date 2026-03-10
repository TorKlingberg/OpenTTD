/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file fixedpoint_func.hpp Deterministic 16.16 fixed-point arithmetic functions. */

#ifndef FIXEDPOINT_FUNC_HPP
#define FIXEDPOINT_FUNC_HPP

#include "math_func.hpp"
#include "../table/fixedpoint_sin_table.h"

/** @name 16.16 fixed-point constants */
/** @{ */
static constexpr int64_t FP16_1        = 1LL << 16;
static constexpr int64_t FP16_PI       = 205887LL;       ///< pi in 16.16
static constexpr int64_t FP16_2PI      = 2 * FP16_PI;    ///< 2*pi in 16.16
static constexpr int64_t FP16_HALF_PI  = 102944LL;       ///< pi/2 in 16.16 (independently rounded)
static constexpr int64_t FP16_EPSILON  = 64;              ///< ~0.001 in 16.16
/** @} */

/** Convert integer to 16.16 fixed-point. */
inline int64_t FP16FromInt(int i) { return (int64_t)i << 16; }

/** Round 16.16 fixed-point to nearest integer. */
inline int FP16Round(int64_t val) { return val >= 0 ? (int)((val + 32768) >> 16) : -(int)((-val + 32768) >> 16); }

/** Multiply two 16.16 fixed-point values. */
inline int64_t FP16Mul(int64_t a, int64_t b) { return (a * b) >> 16; }

/** Divide two 16.16 fixed-point values. */
inline int64_t FP16Div(int64_t a, int64_t b) { return (a << 16) / b; }

/** Normalize angle to [0, 2*pi) in 16.16 fixed-point. */
inline int64_t FP16NormalizeAngle2Pi(int64_t a)
{
	a %= FP16_2PI;
	if (a < 0) a += FP16_2PI;
	return a;
}

/** Fixed-point sine lookup (16.16 in, 16.16 out). */
inline int64_t FP16Sin(int64_t a)
{
	a = FP16NormalizeAngle2Pi(a);
	uint32_t idx = (uint32_t)((a * 4096LL) / FP16_2PI) % 4096;
	return _fixedpoint_sin_table[idx];
}

/** Fixed-point cosine (16.16 in, 16.16 out). */
inline int64_t FP16Cos(int64_t a)
{
	return FP16Sin(a + FP16_HALF_PI);
}

/** Fixed-point square root (16.16 in, 16.16 out). */
inline int64_t FP16Sqrt(int64_t x)
{
	if (x <= 0) return 0;
	return (int64_t)IntSqrt64((uint64_t)x << 16);
}

/**
 * Deterministic Atan2 approximation (16.16).
 * Attempt polynomial approximation: 0.1963*r^3 - 0.9817*r + pi/4 (max error ~0.28 degrees).
 * Constants: 64339 = round(0.9817 * 65536), 12865 = round(0.1963 * 65536).
 */
inline int64_t FP16Atan2(int64_t y, int64_t x)
{
	if (x == 0 && y == 0) return 0;

	const int64_t abs_y = std::abs(y) + 1; /* +1 is sub-epsilon in 16.16 — prevents division by zero */
	int64_t angle;
	if (x >= 0) {
		int64_t r = FP16Div(x - abs_y, x + abs_y);
		int64_t r2 = FP16Mul(r, r);
		int64_t r3 = FP16Mul(r2, r);
		angle = FP16_PI / 4 - FP16Mul(64339, r) + FP16Mul(12865, r3);
	} else {
		int64_t r = FP16Div(x + abs_y, abs_y - x);
		int64_t r2 = FP16Mul(r, r);
		int64_t r3 = FP16Mul(r2, r);
		angle = 3 * FP16_PI / 4 - FP16Mul(64339, r) + FP16Mul(12865, r3);
	}
	if (y < 0) return -angle;
	return angle;
}

#endif /* FIXEDPOINT_FUNC_HPP */
