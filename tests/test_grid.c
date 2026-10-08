/*
 * Tests for grid/region operations.
 */

#include "../src/grid.h"

#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#define ASSERT_REGION(rs, ex, ey, ew, eh)                    \
    do {                                                     \
        assert((rs).current.x == (ex));                      \
        assert((rs).current.y == (ey));                      \
        assert((rs).current.w == (ew));                      \
        assert((rs).current.h == (eh));                      \
    } while (0)

static void test_init(void) {
    struct region_state rs;
    region_init(&rs, 1920, 1080);
    ASSERT_REGION(rs, 0, 0, 1920, 1080);
    assert(rs.current.grid_cols == 2);
    assert(rs.current.grid_rows == 2);
}

static void test_init_clamps_invalid_extent(void) {
    struct region_state rs;
    region_init(&rs, 0, -1);
    ASSERT_REGION(rs, 0, 0, 1, 1);
    assert(rs.current.grid_cols == 1);
    assert(rs.current.grid_rows == 1);
    assert(region_cell_select(&rs, 1));
    ASSERT_REGION(rs, 0, 0, 1, 1);
}

static void test_cell_select_4x4(void) {
    struct region_state rs;
    region_init(&rs, 1600, 1600);
    region_set_grid(&rs, 4, 4);

    /* Cell 1: top-left (col=0, row=0) */
    region_cell_select(&rs, 1);
    ASSERT_REGION(rs, 0, 0, 400, 400);

    /* Reset and try cell 6 (col=1, row=1) */
    region_init(&rs, 1600, 1600);
    region_set_grid(&rs, 4, 4);
    region_cell_select(&rs, 6);
    ASSERT_REGION(rs, 400, 400, 400, 400);

    /* Cell 16: bottom-right (col=3, row=3) */
    region_init(&rs, 1600, 1600);
    region_set_grid(&rs, 4, 4);
    region_cell_select(&rs, 16);
    ASSERT_REGION(rs, 1200, 1200, 400, 400);

    /* Cell 13: (col=3, row=0) */
    region_init(&rs, 1600, 1600);
    region_set_grid(&rs, 4, 4);
    region_cell_select(&rs, 13);
    ASSERT_REGION(rs, 1200, 0, 400, 400);
}

static void test_cut_operations(void) {
    struct region_state rs;
    region_init(&rs, 1000, 1000);

    region_cut_left(&rs);
    ASSERT_REGION(rs, 0, 0, 500, 1000);

    region_init(&rs, 1000, 1000);
    region_cut_right(&rs);
    ASSERT_REGION(rs, 500, 0, 500, 1000);

    region_init(&rs, 1000, 1000);
    region_cut_up(&rs);
    ASSERT_REGION(rs, 0, 0, 1000, 500);

    region_init(&rs, 1000, 1000);
    region_cut_down(&rs);
    ASSERT_REGION(rs, 0, 500, 1000, 500);
}

static void test_move_operations(void) {
    struct region_state rs;
    region_init(&rs, 100, 100);

    /* Start with a 50x50 cell at the top-left. */
    region_cut_left(&rs);
    region_cut_up(&rs);
    ASSERT_REGION(rs, 0, 0, 50, 50);

    region_move_right(&rs);
    ASSERT_REGION(rs, 50, 0, 50, 50);

    region_move_down(&rs);
    ASSERT_REGION(rs, 50, 50, 50, 50);

    region_move_left(&rs);
    ASSERT_REGION(rs, 0, 50, 50, 50);

    region_move_up(&rs);
    ASSERT_REGION(rs, 0, 0, 50, 50);
}

static void test_history(void) {
    struct region_state rs;
    region_init(&rs, 1000, 1000);

    region_save_snapshot(&rs, rs.current);
    region_cut_left(&rs);
    ASSERT_REGION(rs, 0, 0, 500, 1000);

    region_save_snapshot(&rs, rs.current);
    region_cut_up(&rs);
    ASSERT_REGION(rs, 0, 0, 500, 500);

    assert(region_history_back(&rs));
    ASSERT_REGION(rs, 0, 0, 500, 1000);

    assert(region_history_back(&rs));
    ASSERT_REGION(rs, 0, 0, 1000, 1000);

    /* No more history. */
    assert(!region_history_back(&rs));
}

static void test_moves_stay_on_screen(void) {
    struct region_state rs;
    region_init(&rs, 100, 100);

    region_move_left(&rs);
    ASSERT_REGION(rs, 0, 0, 100, 100);

    region_move_up(&rs);
    ASSERT_REGION(rs, 0, 0, 100, 100);

    region_move_right(&rs);
    ASSERT_REGION(rs, 0, 0, 100, 100);

    region_move_down(&rs);
    ASSERT_REGION(rs, 0, 0, 100, 100);

    region_cut_left(&rs);
    region_move_right(&rs);
    ASSERT_REGION(rs, 50, 0, 50, 100);
    region_move_right(&rs);
    ASSERT_REGION(rs, 50, 0, 50, 100);
}

static void test_grid_validation_is_atomic(void) {
    struct region_state rs;
    region_init(&rs, 100, 100);

    assert(!region_set_grid(&rs, 0, 3));
    assert(rs.current.grid_cols == 2);
    assert(rs.current.grid_rows == 2);

    assert(!region_set_grid(&rs, 3, -1));
    assert(rs.current.grid_cols == 2);
    assert(rs.current.grid_rows == 2);

    assert(!region_set_grid(&rs, INT_MAX, 2));
    assert(rs.current.grid_cols == 2);
    assert(rs.current.grid_rows == 2);

    assert(!region_set_grid(&rs, 101, 1));
    assert(rs.current.grid_cols == 2);
    assert(rs.current.grid_rows == 2);
}

static void test_cell_select_matches_render_partitions(void) {
    struct region_state rs;
    region_init(&rs, 100, 100);
    assert(region_set_grid(&rs, 60, 1));

    assert(region_cell_select(&rs, 60));
    ASSERT_REGION(rs, 98, 0, 2, 100);
}

static void test_grid_cells_stay_nonzero(void) {
    struct region_state rs;
    region_init(&rs, 100, 100);
    assert(region_set_grid(&rs, 100, 100));

    region_resize(&rs, 50, 50);
    assert(!region_cell_select(&rs, 1));
    ASSERT_REGION(rs, 0, 0, 50, 50);
}

static void test_cuts_keep_region_nonzero(void) {
    struct region_state rs;
    region_init(&rs, 4, 4);

    for (int i = 0; i < 10; i++) {
        region_cut_left(&rs);
        region_cut_up(&rs);
    }
    ASSERT_REGION(rs, 0, 0, 1, 1);
}

static void test_resize_clamps_region(void) {
    struct region_state rs;
    region_init(&rs, 200, 100);
    region_cut_left(&rs);
    region_cut_left(&rs);
    region_move_right(&rs);
    region_move_right(&rs);

    region_resize(&rs, 120, 80);
    ASSERT_REGION(rs, 70, 0, 50, 80);
}

static void test_history_back_clamps_after_resize(void) {
    struct region_state rs;
    region_init(&rs, 200, 100);
    region_save_snapshot(&rs, rs.current);
    region_resize(&rs, 100, 50);

    assert(region_history_back(&rs));
    ASSERT_REGION(rs, 0, 0, 100, 50);
}

static void test_cursorzoom(void) {
    struct region_state rs;
    region_init(&rs, 1920, 1080);

    assert(region_cursorzoom(&rs, 500, 300, 10, 10));
    ASSERT_REGION(rs, 495, 295, 10, 10);

    assert(region_cursorzoom(&rs, 2, 2, 10, 10));
    ASSERT_REGION(rs, 0, 0, 10, 10);

    assert(region_cursorzoom(&rs, 1919, 1079, 20, 20));
    ASSERT_REGION(rs, 1900, 1060, 20, 20);

    assert(!region_cursorzoom(&rs, 500, 300, 0, 10));
    ASSERT_REGION(rs, 1900, 1060, 20, 20);
}

static void test_center(void) {
    struct region_state rs;
    region_init(&rs, 200, 100);

    int cx, cy;
    region_center(&rs, &cx, &cy);
    assert(cx == 100);
    assert(cy == 50);
}

static void test_cell_select_matches_keynav(void) {
    /* Verify cell numbering matches keynav's scheme.
     * In a 4x4 grid (4 cols, 4 rows), keynav numbers cells 1-based:
     *   col = (cell - 1) / rows
     *   row = (cell - 1) % rows
     *
     * Cell  1: col=0, row=0
     * Cell  2: col=0, row=1
     * Cell  3: col=0, row=2
     * Cell  4: col=0, row=3
     * Cell  5: col=1, row=0
     * Cell  9: col=2, row=0
     * Cell 13: col=3, row=0
     */
    struct region_state rs;

    /* Cell 5 should be col=1, row=0 */
    region_init(&rs, 400, 400);
    region_set_grid(&rs, 4, 4);
    region_cell_select(&rs, 5);
    ASSERT_REGION(rs, 100, 0, 100, 100);

    /* Cell 4 should be col=0, row=3 */
    region_init(&rs, 400, 400);
    region_set_grid(&rs, 4, 4);
    region_cell_select(&rs, 4);
    ASSERT_REGION(rs, 0, 300, 100, 100);

    /* Cell 9 should be col=2, row=0 */
    region_init(&rs, 400, 400);
    region_set_grid(&rs, 4, 4);
    region_cell_select(&rs, 9);
    ASSERT_REGION(rs, 200, 0, 100, 100);
}

int main(void) {
    test_init();
    test_init_clamps_invalid_extent();
    test_cell_select_4x4();
    test_cut_operations();
    test_move_operations();
    test_moves_stay_on_screen();
    test_grid_validation_is_atomic();
    test_cell_select_matches_render_partitions();
    test_grid_cells_stay_nonzero();
    test_cuts_keep_region_nonzero();
    test_resize_clamps_region();
    test_history();
    test_history_back_clamps_after_resize();
    test_cursorzoom();
    test_center();
    test_cell_select_matches_keynav();

    printf("All grid tests passed.\n");
    return 0;
}
