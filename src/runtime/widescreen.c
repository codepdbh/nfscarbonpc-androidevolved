/* widescreen.c - wide-display HUD/FOV adaptation for NFSC.exe.
 *
 * Not mapped for Carbon yet: the game keeps its own geometry and the host
 * only records the requested display and render sizes. Most Wanted's HUD,
 * minimap and resolution sites are other code in Carbon and must not be
 * written here; each Carbon site is measured on NFSC.exe and listed in
 * config/hooks.txt before it is used. */
#include "nfs_widescreen.h"
#include "recomp_types.h"

static float display_aspect, minimap_offset;
static unsigned render_width = 640, render_height = 480;

void nfs_widescreen_init(const char *game_root) { (void)game_root; }
void nfs_widescreen_set_display_aspect(float aspect) { display_aspect = aspect > 1.f && aspect < 8.f ? aspect : 0.f; }
void nfs_widescreen_set_resolution(unsigned width, unsigned height) { render_width = width; render_height = height; }
int nfs_widescreen_hud_entries(void) { return 0; }
void nfs_widescreen_set_minimap_offset(float dy) { minimap_offset = dy; }
float nfs_widescreen_minimap_offset(void) { return minimap_offset; }
void nfs_widescreen_set_frame_rate(unsigned fps) { (void)fps; }
void nfs_widescreen_apply_frame_rate(void) {}
void nfs_frontend_cursor_units(int on) { (void)on; }
float nfs_widescreen_hud_pos_x(void) { return 320.f; }

/* config/hooks.txt has no Carbon sites yet: a hook reaching here is a
 * lift/config mismatch. */
void recomp_hook(uint32_t va) { RECOMP_UNIMPL(va, "hook site without a Carbon handler"); }
