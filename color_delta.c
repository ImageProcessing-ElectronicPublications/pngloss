#include "color_delta.h"

void color_difference(
    int_fast16_t *back_color, int_fast16_t *here_color,
    color_delta difference, uint_fast8_t bytes_per_pixel
) {
    int32_t d;
    switch (bytes_per_pixel) {
        case 1:
            // grayscale
            d = here_color[0] - back_color[0];
            difference[0] = d;
            difference[1] = d;
            difference[2] = d;
            difference[3] = 0;
            break;
        case 2:
            // gray + alpha
            d = here_color[0] - back_color[0];
            difference[0] = d;
            difference[1] = d;
            difference[2] = d;
            difference[3] = here_color[3] - back_color[3];
            break;
        case 3:
            // rgb
            difference[0] = here_color[0] - back_color[0];
            difference[1] = here_color[1] - back_color[1];
            difference[2] = here_color[2] - back_color[2];
            difference[3] = 0;
            break;
        case 4:
            // rgba
            difference[0] = here_color[0] - back_color[0];
            difference[1] = here_color[1] - back_color[1];
            difference[2] = here_color[2] - back_color[2];
            difference[3] = here_color[3] - back_color[3];
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

