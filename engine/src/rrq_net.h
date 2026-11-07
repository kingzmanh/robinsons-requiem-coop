// rrq-coop online co-op (see rrq_net.c).
#pragma once
#include "config.h"
void rrq_net_init(void);
int  rrq_net_active(void);
int  rrq_net_local_player(void);
void rrq_net_frame(void);          // VM thread, end of each scheduler pass
void rrq_net_timer_tick(void);     // SDL timer thread
void rrq_net_local_key(s32 sym, u16 sc);
const u8 *rrq_harness_raw_keys(void);   // real SDL keys + harness-held keys (local PC)
void rrq_net_after_load(void);
void rrq_net_before_save(void);
