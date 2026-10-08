#ifndef WAYNAV_GRID_H
#define WAYNAV_GRID_H

#include <stdbool.h>

#define HISTORY_MAX 100
#define GRID_DIMENSION_MAX 16384

struct region {
    int x, y, w, h;
    int grid_cols, grid_rows;
};

struct region_state {
    struct region current;
    int screen_w;
    int screen_h;
    struct region history[HISTORY_MAX];
    int history_len;
    bool dragging;
    int drag_button;
};

/* Reset to a clamped full-screen region with a fitting default grid. */
void region_init(struct region_state *rs, int scr_w, int scr_h);

/* Push a captured region onto the history stack. Drops the
 * oldest entry when full. */
void region_save_snapshot(struct region_state *rs, struct region snapshot);

/* Restore the most recent history entry. Returns false if
 * the history stack is empty. */
bool region_history_back(struct region_state *rs);

/* Resize the logical screen bounds and clamp the current region. */
void region_resize(struct region_state *rs, int scr_w, int scr_h);

/* Set grid subdivision. Returns false for invalid dimensions or a
 * cell count that overflows int. */
bool region_set_grid(struct region_state *rs, int cols, int rows);

/* Select a cell using keynav's column-major numbering:
 * cell 1 is top-left, cells fill top-to-bottom then
 * left-to-right. Returns false for out-of-range or zero-sized cells. */
bool region_cell_select(struct region_state *rs, int cell);
void region_cut_left(struct region_state *rs);
void region_cut_right(struct region_state *rs);
void region_cut_up(struct region_state *rs);
void region_cut_down(struct region_state *rs);
void region_move_left(struct region_state *rs);
void region_move_right(struct region_state *rs);
void region_move_up(struct region_state *rs);
void region_move_down(struct region_state *rs);
bool region_cursorzoom(struct region_state *rs, int cursor_x, int cursor_y,
                       int w, int h);

/* Center coordinates of the current region. */
void region_center(const struct region_state *rs, int *x, int *y);

#endif /* WAYNAV_GRID_H */
