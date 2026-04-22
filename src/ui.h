#ifndef UI_H
#define UI_H

#include "app.h"

void ui_init(void);
void ui_shutdown(void);
void ui_draw(const App *app);
void ui_get_size(int *rows, int *cols);

#endif
