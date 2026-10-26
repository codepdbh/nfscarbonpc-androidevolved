/* nfs_widescreen.h - wide-display HUD/FOV geometry (see widescreen.c). */
#ifndef NFS_WIDESCREEN_H
#define NFS_WIDESCREEN_H
#ifdef __cplusplus
extern "C" {
#endif

/* Kept for the shared host interface (no data file needed for NFSU1 yet). */
void nfs_widescreen_init(const char* game_root);
/* Display width/height (> 4:3 turns the fix on: render size widened, 3D FOV
 * corrected); 0 keeps the original 4:3 game. */
void nfs_widescreen_set_display_aspect(float aspect);
/* The game's render size; call before its HUD draws and on every change. */
void nfs_widescreen_set_resolution(unsigned width, unsigned height);
/* Touch hosts (Android): the mouse cursor is handed out in front-end units,
 * so its clamp spans the wide HUD space (see nfs_widescreen_hud_pos_x). */
void nfs_frontend_cursor_units(int on);
/* fHudPosX: the FE/HUD centre; the visible FE space is 320 -/+ this value. */
float nfs_widescreen_hud_pos_x(void);
/* HUD offset entries loaded from the .dat (0: elements keep 4:3 anchors). */
int nfs_widescreen_hud_entries(void);
/* Vertical offset (480-line HUD units, negative = up) added to the minimap. */
void nfs_widescreen_set_minimap_offset(float dy);
float nfs_widescreen_minimap_offset(void);
/* Game frame pacing (the game caps itself at 60): fps >= 30 replaces its 1/60 s
 * frame time; 0 keeps the original. apply() re-asserts it once per frame,
 * since the game re-sets 1/60 on mode changes. */
void nfs_widescreen_set_frame_rate(unsigned fps);
void nfs_widescreen_apply_frame_rate(void);

#ifdef __cplusplus
}
#endif
#endif
