#ifndef __MDFN_VIDEO_H
#define __MDFN_VIDEO_H

#include <stdint.h>

#if defined(AURORA_PS2_PCE_FAST)
/* AURORA_PCE_GS_NATIVE_5551
 * AURORA_PCE_EXPERIMENTAL_V12_NATIVE_GS_COLOR
 *
 * Dedicated PS2/Aurora PCE build: emit the GS PSMCT16 bit layout directly.
 * GS CT16 is A1B5G5R5 in the 16-bit word:
 *   R = bits 0..4, G = 5..9, B = 10..14, A = bit 15.
 *
 * This is intentionally 5:5:5 rather than RGB565.  It is bit-identical to
 * V11's proven RGB565->GS conversion, but happens when the 512-entry VCE
 * palette is built instead of once per output pixel per frame.
 */
#define RED_MASK    0x001f
#define GREEN_MASK  0x03e0
#define BLUE_MASK   0x7c00
#define RED_EXPAND  3
#define GREEN_EXPAND 3
#define BLUE_EXPAND 3
#define RED_SHIFT   0
#define GREEN_SHIFT 5
#define BLUE_SHIFT  10
#define MAKECOLOR(r, g, b, a) \
   ((uint16_t)(0x8000u | \
   (((uint16_t)(r) >> RED_EXPAND) << RED_SHIFT) | \
   (((uint16_t)(g) >> GREEN_EXPAND) << GREEN_SHIFT) | \
   (((uint16_t)(b) >> BLUE_EXPAND) << BLUE_SHIFT)))
#elif defined(FRONTEND_SUPPORTS_RGB565)
/* 16bit color - RGB565 */
#define RED_MASK  0xf800
#define GREEN_MASK 0x7e0
#define BLUE_MASK 0x1f
#define RED_EXPAND 3
#define GREEN_EXPAND 2
#define BLUE_EXPAND 3
#define RED_SHIFT 11
#define GREEN_SHIFT 5
#define BLUE_SHIFT 0
#define MAKECOLOR(r, g, b, a) (((r >> RED_EXPAND) << RED_SHIFT) | ((g >> GREEN_EXPAND) << GREEN_SHIFT) | ((b >> BLUE_EXPAND) << BLUE_SHIFT))
#else
/* 16bit color - RGB555 */
#define RED_MASK  0x7c00
#define GREEN_MASK 0x3e0
#define BLUE_MASK 0x1f
#define RED_EXPAND 3
#define GREEN_EXPAND 3
#define BLUE_EXPAND 3
#define RED_SHIFT 10
#define GREEN_SHIFT 5
#define BLUE_SHIFT 0
#define MAKECOLOR(r, g, b, a) (((r >> RED_EXPAND) << RED_SHIFT) | ((g >> GREEN_EXPAND) << GREEN_SHIFT) | ((b >> BLUE_EXPAND) << BLUE_SHIFT))
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
 int32_t x, y, w, h;
} MDFN_Rect;

typedef struct
{
   unsigned int colorspace;
   uint8_t r_shift;
   uint8_t g_shift;
   uint8_t b_shift;
   uint8_t a_shift;
} MDFN_PixelFormat;

typedef struct
{
   uint16_t *pixels;
   int32_t width;
   int32_t height;
   int32_t pitch;
} MDFN_Surface;

#ifdef __cplusplus
}
#endif

#endif
