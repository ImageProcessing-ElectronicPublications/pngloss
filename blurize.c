/**
 Blurizer
 © 2020 William MacKay.
 © 2013 Kornel Lesiński.
 Based on algorithms by Michael Vinther and William MacKay.

 This program is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 any later version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 See the GNU General Public License for more details:
 <http://www.gnu.org/copyleft/gpl.html>
*/

#include <png.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "rwpng.h"

const uint_fast8_t error_row_count = 3;
const uint_fast8_t filter_width = 5;

typedef int32_t color_delta[4];

typedef struct {
    uint32_t x, y;
    unsigned char *sliding_window;
    uint32_t sliding_index;
    unsigned char *pixels;
    color_delta *color_error;
} optimize_state;

pngloss_error optimize_state_init(optimize_state *state, uint32_t width, uint32_t sliding_length, uint_fast8_t bytes_per_pixel);
void optimize_state_destroy(optimize_state *state);
uint_fast8_t adaptive_filter_for_row(
    unsigned char **rows, uint32_t width,
    uint32_t y, uint_fast8_t bytes_per_pixel
);
void diffuse_color_deltas(color_delta **color_error, uint32_t x, int32_t *delta);
void color_difference(
    unsigned char *back_color, unsigned char *here_color,
    color_delta difference, uint_fast8_t bytes_per_pixel
);
uint32_t color_distance(color_delta difference);

pngloss_error optimize_state_init(optimize_state *state, uint32_t width, uint_fast16_t sliding_length, uint_fast8_t bytes_per_pixel) {
    state->x = 0;
    state->y = 0;
    state->sliding_index = 0;

    // clear values in case we return early and later free uninitialized pointers
    state->sliding_window = NULL;
    state->pixels = NULL;
    state->color_error = NULL;

    // multiply by two to leave space for saved duplicate copy
    state->sliding_window = calloc((size_t)sliding_length * 2, sizeof(unsigned char));
    if (!state->sliding_window) {
        return OUT_OF_MEMORY_ERROR;
    }

    // don't multiply by two because main copy is in rows instead
    state->pixels = calloc((size_t)width, bytes_per_pixel);
    if (!state->pixels) {
        return OUT_OF_MEMORY_ERROR;
    }

    uint32_t error_width = width + filter_width - 1;
    // multiply by two to leave space for saved duplicate copy
    state->color_error = calloc((size_t)error_width * error_row_count * 2, sizeof(color_delta));
    if (!state->color_error) {
        return OUT_OF_MEMORY_ERROR;
    }

    return SUCCESS;
}

void optimize_state_destroy(optimize_state *state) {
    free(state->sliding_window);
    free(state->pixels);
    free(state->color_error);
}

void optimize_state_copy(optimize_state *from, optimize_state *to, uint32_t width, uint_fast16_t sliding_length, uint_fast8_t bytes_per_pixel) {
    to->x = from->x;
    to->y = from->y;

    memcpy(to->sliding_window, from->sliding_window, (size_t)sliding_length * 2);
    to->sliding_index = from->sliding_index;

    memcpy(to->pixels, from->pixels, (size_t)width * bytes_per_pixel);
    to->sliding_index = from->sliding_index;

    uint32_t error_width = width + filter_width - 1;
    memcpy(to->color_error, from->color_error, (size_t)error_width * error_row_count * 2 * sizeof(color_delta));
}

uint_fast8_t optimize_state_average_run(
    optimize_state *state, unsigned char **rows,
    uint32_t width, uint_fast16_t sliding_length,
    int_fast16_t quantization, uint_fast8_t bytes_per_pixel
) {
    // max_run_length and run_length are in units of pixels. 64 is the max
    // effective value for RGBA because DEFLATE has maximum back- reference
    // length of 258. Lower values cause faster but less effective
    // compression. Higher values marginally increase compression for opaque
    // or grayscale images at the expense of speed.
    uint_fast16_t max_run_length = 64;
    // Don't go farther than the end of the line because that's when we
    // compare the different filter types and decide which to keep.
    uint32_t until_line_end = width - state->x;
    if (max_run_length > until_line_end) {
        max_run_length = until_line_end;
    }

    //printf("In %s, width == %d, height == %d, sliding_length == %d, quantization == %d, bytes_per_pixel == %d\n", __func__, (int)width, (int)height, (int)sliding_length, (int)quantization, (int)bytes_per_pixel);
    // save copy of sliding window to restore after failed match
    uint32_t saved_x = state->x;
    memcpy(state->sliding_window + sliding_length, state->sliding_window, sliding_length);
    uint32_t saved_sliding_index = state->sliding_index;
    uint32_t sliding_dirty_left = sliding_length - 1;
    uint32_t sliding_dirty_right = 0;

    memcpy(state->pixels, rows[state->y], width * bytes_per_pixel);
    uint32_t pixels_dirty_left = width - 1;
    uint32_t pixels_dirty_right = 0;

    // start with best-case length and distance and back off until match found
    for (uint_fast16_t run_length = max_run_length; run_length > 0; run_length--) {
        for (uint_fast16_t distance = 1; distance < sliding_length; distance++) {
            // save copy of x to restore after failed match
            // not necessary to save y because won't span rows
            bool match_failed = false;
            unsigned long max_error = quantization * quantization * (unsigned long)(sliding_length - 1);
            unsigned long total_error = 0;
            for (uint_fast16_t back_index = 0; back_index < run_length; back_index++) {
                unsigned char back_color[4], here_color[4];
                //printf("run_length == %d, distance == %d, x == %d, max_error == %d, total_error == %d, back_index == %d\n", (int)run_length, (int)distance, (int)state->x, (int)max_error, (int)total_error, (int)back_index);
                for (uint_fast8_t c = 0; c < bytes_per_pixel; c++) {
                    unsigned char back_value = state->sliding_window[(sliding_length + state->sliding_index - distance) % sliding_length];
                    state->sliding_window[state->sliding_index] = back_value;
                    if (sliding_dirty_left > state->sliding_index) {
                        sliding_dirty_left = state->sliding_index;
                    }
                    if (sliding_dirty_right < state->sliding_index) {
                        sliding_dirty_right = state->sliding_index;
                    }
                    state->sliding_index = (state->sliding_index + 1) % sliding_length;

                    uint32_t offset = state->x*bytes_per_pixel + c;
                    unsigned char above = 0, left = 0;
                    if (state->y > 0) {
                        above = rows[state->y-1][offset];
                    }
                    if (state->x > 0) {
                        left = rows[state->y][offset-bytes_per_pixel];
                    }
                    back_color[c] = back_value + (above + left) / 2;

                    here_color[c] = rows[state->y][offset];
                    rows[state->y][offset] = back_color[c];
                    if (pixels_dirty_left > state->x) {
                        pixels_dirty_left = state->x;
                    }
                    if (pixels_dirty_right < state->x) {
                        pixels_dirty_right = state->x;
                    }
                }
                state->x++;

                color_delta difference;
                color_difference(back_color, here_color, difference, bytes_per_pixel);
                uint32_t color_error = color_distance(difference);
                //printf("In %s, total_error == %d, color_error == %d, difference == {%d, %d, %d, %d}\n", __func__, (int)total_error, (int)color_error, (int)difference[0], (int)difference[1], (int)difference[2], (int)difference[3]);
                total_error += (unsigned long)color_error;
                if (color_error > max_error || total_error > max_error) {
                    // too much error, abort this run length + distance and try another
                    match_failed = true;
                    //printf("In %s, failed match with run_length == %d, average_error == %d, distance == %d, reverting to x == %d\n", __func__, (int)run_length, (int)(total_error / (state->x - saved_x)), (int)distance, (int)saved_x);
                    // restore saved x coordinate
                    state->x = saved_x;
                    // restore saved sliding window
                    state->sliding_index = saved_sliding_index;
                    if (!(sliding_dirty_left > sliding_dirty_right)) {
                        memcpy(state->sliding_window + sliding_dirty_left, state->sliding_window + sliding_length + sliding_dirty_left, sliding_dirty_right - sliding_dirty_left + 1);
                    }
                    sliding_dirty_left = sliding_length - 1;
                    sliding_dirty_right = 0;
                    if (!(pixels_dirty_left > pixels_dirty_right)) {
                        memcpy(rows[state->y] + pixels_dirty_left * bytes_per_pixel, state->pixels + pixels_dirty_left * bytes_per_pixel, (pixels_dirty_right - pixels_dirty_left + 1) * bytes_per_pixel);
                    }
                    pixels_dirty_left = width - 1;
                    pixels_dirty_right = 0;
                    break;
                }

                // looking good so far, continue to next pixel in run
            }
            if (!match_failed) {
                // found matching run
                //printf("Found match at %u, %u, run_length == %u, distance == %u\n", (unsigned int)state->x, (unsigned int)state->y, (unsigned int)run_length, (unsigned int)distance);
                uint_fast8_t log2 = 0;
                while (distance) {
                    distance >>= 1;
                    log2++;
                }
                if (log2 > 2) {
                    log2 -= 2;
                } else {
                    log2 = 0;
                }
                return 8 + log2;
            }
        }
    }
    // found no matching run, give up and insert pixel as-is (uncompressed)
    for (uint_fast8_t c = 0; c < bytes_per_pixel; c++) {
        uint32_t offset = state->x*bytes_per_pixel + c;
        unsigned char above = 0, left = 0;
        if (state->y > 0) {
            above = rows[state->y-1][offset];
        }
        if (state->x > 0) {
            left = rows[state->y][offset-bytes_per_pixel];
        }

        unsigned char here_color = rows[state->y][offset];
        unsigned char average_value = here_color - (above + left) / 2;

        state->sliding_window[state->sliding_index] = average_value;
        state->sliding_index = (state->sliding_index + 1) % sliding_length;
    }
    state->x++;
    //printf("Inserting pixel at %u, %u, sliding_index == %u\n", (unsigned int)state->x, (unsigned int)state->y, (unsigned int)state->sliding_index);
    return 8;
}

unsigned long optimize_state_average_row(
    optimize_state *state, unsigned char **rows,
    uint32_t width, uint32_t sliding_length,
    int_fast16_t quantization, uint_fast8_t bytes_per_pixel
) {
    //printf("In %s, width == %d, height == %d, sliding_length == %d, quantization == %d, bytes_per_pixel == %d\n", __func__, (int)width, (int)height, (int)sliding_length, (int)quantization, (int)bytes_per_pixel);
    unsigned long total_cost = 0;
    while (state->x < width) {
        uint_fast8_t cost = optimize_state_average_run(state, rows, width, sliding_length, quantization, bytes_per_pixel);
        total_cost += cost;
        //printf("In %s, cost == %d, total_cost == %d\n", __func__, (int)cost, (int)total_cost);
    }

    uint_fast8_t filter = adaptive_filter_for_row(rows, width, state->y, bytes_per_pixel);
    printf("finished row %u, total_cost == %lu, filter == %u\n", (unsigned int)state->y, total_cost, (unsigned int)filter);

    state->x = 0;
    state->y++;

    if (PNG_FILTER_AVG != filter) {
        return 0;
    }
    return total_cost;
}

uint_fast8_t adaptive_filter_for_row(
    unsigned char **rows, uint32_t width,
    uint32_t y, uint_fast8_t bytes_per_pixel
) {
    uint32_t none_sum = 0, sub_sum = 0, up_sum = 0;
    uint32_t average_sum = 0, paeth_sum = 0;

    for (uint32_t i = 0; i < width * bytes_per_pixel; i++) {
        unsigned char above = 0, left = 0, diag = 0;
        if (i >= bytes_per_pixel) {
            left = rows[y][i-bytes_per_pixel];
            if (y > 0) {
                diag = rows[y-1][i-bytes_per_pixel];
            }
        }
        if (y > 0) {
            above = rows[y-1][i];
        }
        unsigned char here = rows[y][i];
        none_sum += (here < 128) ? here : (256 - here);

        unsigned char sub = here - left;
        sub_sum += (sub < 128) ? sub : (256 - sub);

        unsigned char up = here - above;
        up_sum += (up < 128) ? up : (256 - up);

        unsigned char average = here - (left + above) / 2;
        average_sum += (average < 128) ? average : (256 - average);

        int_fast16_t p = above - diag;
        int_fast16_t p_diag = left - diag;
        int_fast16_t p_left = p < 0 ? -p : p;
        int_fast16_t p_above = p_diag < 0 ? -p_diag : p_diag;
        p_diag = (p + p_diag) < 0 ? -(p + p_diag) : p + p_diag;
        int_fast16_t predicted = (p_left <= p_above && p_left <= p_diag) ? left : (p_above <= p_diag) ? above : diag;
        unsigned char paeth = here - predicted;

        paeth_sum += (paeth < 128) ? paeth : 256 - paeth;
    }
    uint32_t min_sum = none_sum;
    if (min_sum > sub_sum) {
        min_sum = sub_sum;
    }
    if (min_sum > up_sum) {
        min_sum = up_sum;
    }
    if (min_sum > average_sum) {
        min_sum = average_sum;
    }
    if (min_sum > paeth_sum) {
        min_sum = paeth_sum;
    }

    if (min_sum >= none_sum) {
        return PNG_FILTER_NONE;
    }
    if (min_sum >= sub_sum) {
        return PNG_FILTER_SUB;
    }
    if (min_sum >= up_sum) {
        return PNG_FILTER_UP;
    }
    if (min_sum >= average_sum) {
        return PNG_FILTER_AVG;
    }
    if (min_sum >= paeth_sum) {
        return PNG_FILTER_PAETH;
    }
    // unreachable
    return PNG_NO_FILTERS;
}

pngloss_error optimize_with_rows(
    unsigned char **rows, uint32_t width, uint32_t height,
    uint32_t sliding_length, int_fast16_t quantization,
    uint_fast8_t bytes_per_pixel
) {
    //printf("In %s, width == %d, height == %d, sliding_length == %d, quantization == %d, bytes_per_pixel == %d\n", __func__, (int)width, (int)height, (int)sliding_length, (int)quantization, (int)bytes_per_pixel);

    optimize_state average_state;
    pngloss_error retval;

    retval = optimize_state_init(&average_state, width, sliding_length, bytes_per_pixel);
    if (SUCCESS == retval) {
        while (average_state.y < height) {
            unsigned long average_cost = optimize_state_average_row(
                &average_state, rows, width, sliding_length,
                quantization, bytes_per_pixel
            );
            //printf("average_cost == %lu\n", average_cost);
        }
    }
    optimize_state_destroy(&average_state);

    return retval;
    /*
    int_fast16_t half_step = quantization / 2;

    const uint_fast8_t filter_center = 2;
    const int_fast16_t dithering_precision = 256;
    uint32_t error_width = width + filter_width - 1;
    color_delta *error_rows = calloc(error_width * error_row_count, sizeof(color_delta));
    if (!error_rows) {
        return OUT_OF_MEMORY_ERROR;
    }
    color_delta *color_error[error_row_count];
    for (uint_fast8_t i = 0; i < error_row_count; i++) {
        color_error[i] = error_rows + i*error_width;
    }

    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            color_delta diffusion = {0,0,0,0};
            diffuse_color_deltas(color_error, x + filter_center, diffusion);
            for (uint_fast8_t c = 0; c < bytes_per_pixel; c++) {
                uint32_t offset = x*bytes_per_pixel + c;
                int32_t here = rows[y][offset];
                int32_t error_here = 0;
                if (here > 0 && here < 255) {
                    int32_t above = 0, left = 0;
                    if (y > 0) {
                        above = rows[y-1][offset];
                    }
                    if (x > 0) {
                        left = rows[y][offset-bytes_per_pixel];
                    }
                    int32_t average = (above + left) / 2; // PNG average filter

                    int32_t new_value = diffusion[c]/dithering_precision + here - average;
                    new_value += half_step;
                    new_value -= new_value % quantization;
                    new_value += average;
                    if (new_value >= 0 && new_value <= 255) {
                        rows[y][offset] = new_value;
                        error_here = here - new_value;
                    }
                    color_error[0][x + filter_center][c] = error_here * dithering_precision;
                }
            }
        }
        color_delta *color_temp = color_error[error_row_count-1];
        for(int_fast8_t i = error_row_count-1; i > 0; i--) {
            color_error[i] = color_error[i-1];
        }
        color_error[0] = color_temp;
        for (uint32_t i = 0; i < error_width; i++) {
            color_error[0][i][0] = 0;
            color_error[0][i][1] = 0;
            color_error[0][i][2] = 0;
            color_error[0][i][3] = 0;
        }
    }
    free(error_rows);
    return SUCCESS;
    */
}

void diffuse_color_deltas(color_delta **color_error, uint32_t x, int32_t *delta) {
    // Sierra dithering
    for(uint_fast8_t i = 0; i < 4; i++) {
        delta[i] += 2 * color_error[2][x-1][i];
        delta[i] += 3 * color_error[2][x][i];
        delta[i] += 2 * color_error[2][x+1][i];
        delta[i] += 2 * color_error[1][x-2][i];
        delta[i] += 4 * color_error[1][x-1][i];
        delta[i] += 5 * color_error[1][x][i];
        delta[i] += 4 * color_error[1][x+1][i];
        delta[i] += 2 * color_error[1][x+2][i];
        delta[i] += 3 * color_error[0][x-2][i];
        delta[i] += 5 * color_error[0][x-1][i];
        if (delta[i] < 0) {
            delta[i] -= 16;
        } else {
            delta[i] += 16;
        }
        delta[i] /= 32;
    }
}

void color_difference(
    unsigned char *back_color, unsigned char *here_color,
    color_delta difference, uint_fast8_t bytes_per_pixel
) {
    int32_t d;
    switch (bytes_per_pixel) {
        case 1:
            // grayscale
            d = (int32_t)here_color[0] - (int32_t)back_color[0];
            difference[0] = d;
            difference[1] = d;
            difference[2] = d;
            difference[3] = 0;
            break;
        case 2:
            // gray + alpha
            d = (int32_t)here_color[0] - (int32_t)back_color[0];
            difference[0] = d;
            difference[1] = d;
            difference[2] = d;
            difference[3] = (int32_t)here_color[3] - (int32_t)back_color[3];
            break;
        case 3:
            // rgb
            difference[0] = (int32_t)here_color[0] - (int32_t)back_color[0];
            difference[1] = (int32_t)here_color[1] - (int32_t)back_color[1];
            difference[2] = (int32_t)here_color[2] - (int32_t)back_color[2];
            difference[3] = 0;
            break;
        case 4:
            // rgba
            difference[0] = (int32_t)here_color[0] - (int32_t)back_color[0];
            difference[1] = (int32_t)here_color[1] - (int32_t)back_color[1];
            difference[2] = (int32_t)here_color[2] - (int32_t)back_color[2];
            difference[3] = (int32_t)here_color[3] - (int32_t)back_color[3];
            break;
    }
}

uint32_t color_distance(color_delta difference) {
    uint32_t total = 0;
    for (uint_fast8_t i = 0; i < 4; i++) {
        total += difference[i] * difference[i];
    }
    return total;
}

void optimize_with_stride(
    unsigned char *pixels, uint32_t width, uint32_t height,
    uint32_t stride, uint32_t sliding_length,
    int_fast16_t quantization, uint_fast8_t bytes_per_pixel
) {
    unsigned char **rows = malloc((size_t)height * sizeof(unsigned char *));
    for (uint32_t i = 0; i < height; i++) {
        rows[i] = pixels + i*stride;
    }
    optimize_with_rows(rows, width, height, sliding_length, quantization, bytes_per_pixel);
    free(rows);
}

void optimizeForAverageFilter(
    unsigned char *pixels, int width, int height, int quantization
) {
    const uint_fast8_t bytes_per_pixel = 4;
    uint32_t stride = width * bytes_per_pixel;

    const uint32_t sliding_length = 1000;
    optimize_with_stride(pixels, width, height, stride, sliding_length, quantization, bytes_per_pixel);
}

