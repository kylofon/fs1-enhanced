/* Display-list recorder, see displaylist.h and docs/RENDER.md. S1.1 skeleton: the hooks are not
 * placed yet and dl_on stays false; S1.2 fills these in. */
#include "enhanced/displaylist.h"

bool dl_on;

static DlFrame frames[2]; /* building, committed */
static int building;
static bool committed;

const DlFrame *dl_view(void) { return committed ? &frames[building ^ 1] : NULL; }

void dl_stage_line_(Pc *pc, bool from_prev) { (void)pc, (void)from_prev; }
void dl_stage_dot_(Pc *pc, uint16_t x, uint16_t y, uint16_t z) { (void)pc, (void)x, (void)y, (void)z; }
void dl_capture_(Pc *pc, uint16_t addr) { (void)pc, (void)addr; }
void dl_stage_from_capture_(Pc *pc, uint16_t addr) { (void)pc, (void)addr; }
void dl_line_(Pc *pc, int x0, int y0, int x1, int y1, uint16_t seg) { (void)pc, (void)x0, (void)y0, (void)x1, (void)y1, (void)seg; }
void dl_dot_(Pc *pc, int x, int y, uint16_t seg) { (void)pc, (void)x, (void)y, (void)seg; }
void dl_fill_view_(Pc *pc, uint16_t pattern) { (void)pc, (void)pattern; }
void dl_fill_rows_(Pc *pc, int first, int last, uint16_t pattern, uint16_t seg) { (void)pc, (void)first, (void)last, (void)pattern, (void)seg; }
void dl_fill_rect_(Pc *pc, int x, int y, int w, int h, uint16_t pattern, uint16_t seg) { (void)pc, (void)x, (void)y, (void)w, (void)h, (void)pattern, (void)seg; }
void dl_sky_ground_(Pc *pc) { (void)pc; }
void dl_poly_begin_(Pc *pc) { (void)pc; }
void dl_poly_end_(Pc *pc, bool filled) { (void)pc, (void)filled; }

void dl_commit_view_(Pc *pc)
{
    (void)pc;
    frames[building].number = frames[building ^ 1].number + 1;
    building ^= 1;
    frames[building].n_items = frames[building].n_verts = 0;
    frames[building].incomplete = false;
    committed = true;
}
