// rrq-coop: --make-icon (see rrq_icon.c)
#pragma once
void rrq_icon_setup(const char *out_ico, const char *exe_to_patch);
int  rrq_icon_mode(void);
int  rrq_icon_menu_seen(void);
void rrq_icon_after_draw(void);    // VM thread, after draw()
