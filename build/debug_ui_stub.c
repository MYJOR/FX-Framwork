#include "debug_ui.h"
#include <string.h>
int debug_ui_init(DebugState *s){ memset(s,0,sizeof *s); s->draw_streaks=s->draw_dust=s->draw_player=s->draw_terrain=1; return 0; }
void debug_ui_shutdown(void){}
void debug_ui_event(void *u,const PlatEvent *e){(void)u;(void)e;}
void debug_ui_frame(DebugState *s,DebugWorld *w,int a,int b,float d){(void)s;(void)w;(void)a;(void)b;(void)d;}
int debug_ui_wants_mouse(const DebugState *s){(void)s;return 0;}
