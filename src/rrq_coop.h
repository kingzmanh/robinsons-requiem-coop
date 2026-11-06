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
