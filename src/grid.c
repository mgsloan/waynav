/*
 * Region/grid state and manipulation.
 *
 * The region is a rectangle on screen. Grid operations subdivide
 * it, cut/move shift it, and history allows undo.
 */

#include "grid.h"
#include "memory-util.h"
#include <limits.h>
#include <string.h>

void region_init(struct region_state *rs, int scr_w, int scr_h) {
    if (scr_w < 1)
        scr_w = 1;
    if (scr_h < 1)
        scr_h = 1;

    ZERO_OBJECT(*rs);
    rs->current.x = 0;
    rs->current.y = 0;
    rs->current.w = scr_w;
    rs->current.h = scr_h;
    rs->current.grid_cols = scr_w > 1 ? 2 : 1;
    rs->current.grid_rows = scr_h > 1 ? 2 : 1;
    rs->screen_w = scr_w;
    rs->screen_h = scr_h;
    rs->history_len = 0;
    rs->dragging = false;
    rs->drag_button = 0;
}

void region_save_snapshot(struct region_state *rs, struct region snapshot) {
    if (rs->history_len >= HISTORY_MAX) {
        memmove(&rs->history[0], &rs->history[1],
                (HISTORY_MAX - 1) * sizeof(struct region));
        rs->history_len = HISTORY_MAX - 1;
    }
    rs->history[rs->history_len] = snapshot;
    rs->history_len++;
}

static void region_clamp(struct region_state *rs) {
    if (rs->current.w > rs->screen_w)
        rs->current.w = rs->screen_w;
    if (rs->current.h > rs->screen_h)
        rs->current.h = rs->screen_h;

    if (rs->current.x < 0)
        rs->current.x = 0;
    if (rs->current.y < 0)
        rs->current.y = 0;
    if (rs->current.x > rs->screen_w - rs->current.w)
        rs->current.x = rs->screen_w - rs->current.w;
    if (rs->current.y > rs->screen_h - rs->current.h)
        rs->current.y = rs->screen_h - rs->current.h;
}

bool region_history_back(struct region_state *rs) {
    if (rs->history_len <= 0)
        return false;
    rs->history_len--;
    rs->current = rs->history[rs->history_len];
    region_clamp(rs);
    return true;
}

void region_resize(struct region_state *rs, int scr_w, int scr_h) {
    if (scr_w <= 0 || scr_h <= 0)
        return;
    rs->screen_w = scr_w;
    rs->screen_h = scr_h;
    region_clamp(rs);
}

bool region_set_grid(struct region_state *rs, int cols, int rows) {
    int cols_max =
        rs->screen_w < GRID_DIMENSION_MAX ? rs->screen_w : GRID_DIMENSION_MAX;
    int rows_max =
        rs->screen_h < GRID_DIMENSION_MAX ? rs->screen_h : GRID_DIMENSION_MAX;
    if (cols <= 0 || rows <= 0 || cols > cols_max || rows > rows_max ||
        cols > INT_MAX / rows)
        return false;
    rs->current.grid_cols = cols;
    rs->current.grid_rows = rows;
    return true;
}

bool region_cell_select(struct region_state *rs, int cell) {
    int cols = rs->current.grid_cols;
    int rows = rs->current.grid_rows;
    if (cell < 1 || cell > cols * rows)
        return false;

    /* keynav numbering: top-to-bottom within a column, then
     * left-to-right across columns. Cells are 1-based, so the
     * column and row are derived from cell - 1. */
    int col = (cell - 1) / rows;
    int row = (cell - 1) % rows;

    int x0 = rs->current.x + (int)((long long)rs->current.w * col / cols);
    int x1 = rs->current.x + (int)((long long)rs->current.w * (col + 1) / cols);
    int y0 = rs->current.y + (int)((long long)rs->current.h * row / rows);
    int y1 = rs->current.y + (int)((long long)rs->current.h * (row + 1) / rows);
    if (x1 <= x0 || y1 <= y0)
        return false;

    rs->current.x = x0;
    rs->current.y = y0;
    rs->current.w = x1 - x0;
    rs->current.h = y1 - y0;
    return true;
}

void region_cut_left(struct region_state *rs) {
    if (rs->current.w > 1)
        rs->current.w /= 2;
}

void region_cut_right(struct region_state *rs) {
    int orig = rs->current.w;
    if (orig <= 1)
        return;
    rs->current.w /= 2;
    rs->current.x += orig - rs->current.w;
}

void region_cut_up(struct region_state *rs) {
    if (rs->current.h > 1)
        rs->current.h /= 2;
}

void region_cut_down(struct region_state *rs) {
    int orig = rs->current.h;
    if (orig <= 1)
        return;
    rs->current.h /= 2;
    rs->current.y += orig - rs->current.h;
}

void region_move_left(struct region_state *rs) {
    rs->current.x -= rs->current.w;
    region_clamp(rs);
}

void region_move_right(struct region_state *rs) {
    rs->current.x += rs->current.w;
    region_clamp(rs);
}

void region_move_up(struct region_state *rs) {
    rs->current.y -= rs->current.h;
    region_clamp(rs);
}

void region_move_down(struct region_state *rs) {
    rs->current.y += rs->current.h;
    region_clamp(rs);
}

bool region_cursorzoom(struct region_state *rs, int cursor_x, int cursor_y,
                       int w, int h) {
    if (w <= 0 || h <= 0)
        return false;
    if (w > rs->screen_w)
        w = rs->screen_w;
    if (h > rs->screen_h)
        h = rs->screen_h;

    rs->current.x = cursor_x - w / 2;
    rs->current.y = cursor_y - h / 2;
    rs->current.w = w;
    rs->current.h = h;
    region_clamp(rs);
    return true;
}

void region_center(const struct region_state *rs, int *x, int *y) {
    *x = rs->current.x + rs->current.w / 2;
    *y = rs->current.y + rs->current.h / 2;
}
