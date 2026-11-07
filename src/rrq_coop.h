// rrq-coop core: per-player main-variable records (see rrq_coop.c).
#pragma once
int  rrq_coop_active_player(void);
void rrq_coop_switch(int p);                 // VM thread only
void rrq_coop_request_player(int p);         // any thread; applied by rrq_coop_service
void rrq_coop_request_poke(int off, int val, int is_word);
void rrq_coop_service(void);                 // VM thread, scheduler safe point
void rrq_coop_request_spawn(const char *name, int owner);
void rrq_coop_request_drive(int v);
void rrq_coop_slice_begin(void);
void rrq_coop_request_p2view(const char *name);
int  rrq_coop_owner(unsigned int vram);
void rrq_coop_before_draw(void);
void rrq_coop_after_draw(void);
int  rrq_coop_split(void);
int  rrq_coop_p2_frame(unsigned int *dst, int w, int h);
void rrq_coop_slice_end(void);
void rrq_coop_request_dump(const char *t);
void rrq_coop_request_avatar(const char *script, int res, int dx, int dy);
void rrq_coop_request_npcavatar(const char *script);
void rrq_coop_start(void);
