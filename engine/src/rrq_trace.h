// rrq-coop analysis tracing (see rrq_trace.c).
#pragma once
void rrq_trace_slice_begin(void);
void rrq_trace_slice_end(void);
void rrq_trace_mark(const char *text);
void rrq_trace_dump_entities(const char *tag);
void rrq_trace_dump_context(const char *name);
void rrq_trace_dump_sprites(void);
