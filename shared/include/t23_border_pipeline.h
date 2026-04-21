#ifndef T23_BORDER_PIPELINE_H
#define T23_BORDER_PIPELINE_H

#include <stdint.h>

/*
 * Shared data model for TV border calibration.
 *
 * What is active today:
 * - 8-point calibration storage (TL/TM/TR/RM/BR/BM/BL/LM)
 * - common RGB cell type used by T23/C3 transport and sampling code
 * - legacy border-layout constants kept only for compatibility with old
 *   DEBUG-mode helpers
 *
 * What is no longer the main product path:
 * - fixed 50-block extraction as the primary runtime output
 *
 * The current runtime path is:
 * - T23 rectifies into a logical 16x16 mosaic
 * - C3 remaps that 16x16 mosaic to the physical LED strip
 */

#define T23_BORDER_POINT_COUNT 8u
#define T23_BORDER_BLOCK_COUNT_MAX 50u

#define T23_BORDER_LAYOUT_16X9_TOP 16u
#define T23_BORDER_LAYOUT_16X9_RIGHT 9u
#define T23_BORDER_LAYOUT_16X9_BOTTOM 16u
#define T23_BORDER_LAYOUT_16X9_LEFT 9u

#define T23_BORDER_LAYOUT_4X3_TOP 4u
#define T23_BORDER_LAYOUT_4X3_RIGHT 3u
#define T23_BORDER_LAYOUT_4X3_BOTTOM 4u
#define T23_BORDER_LAYOUT_4X3_LEFT 3u

typedef enum {
    /* top-left corner */
    T23_BORDER_POINT_TL = 0,
    /* top edge midpoint */
    T23_BORDER_POINT_TM = 1,
    /* top-right corner */
    T23_BORDER_POINT_TR = 2,
    /* right edge midpoint */
    T23_BORDER_POINT_RM = 3,
    /* bottom-right corner */
    T23_BORDER_POINT_BR = 4,
    /* bottom edge midpoint */
    T23_BORDER_POINT_BM = 5,
    /* bottom-left corner */
    T23_BORDER_POINT_BL = 6,
    /* left edge midpoint */
    T23_BORDER_POINT_LM = 7,
} t23_border_point_id_t;

typedef enum {
    T23_BORDER_LAYOUT_16X9 = 0,
    T23_BORDER_LAYOUT_4X3 = 1,
} t23_border_layout_t;

typedef struct {
    int16_t x;
    int16_t y;
} t23_border_point_t;

typedef struct {
    uint16_t image_width;
    uint16_t image_height;
    t23_border_point_t points[T23_BORDER_POINT_COUNT];
} t23_border_calibration_t;

typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
} t23_rgb8_t;

typedef struct {
    uint8_t block_index;
    t23_rgb8_t color;
} t23_border_block_t;

#endif
