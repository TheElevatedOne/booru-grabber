#pragma once

#include <gtk/gtk.h>

namespace bg::gui {

void installStyle();
void updateDynamicStyle();
GdkRGBA themeColor(GtkWidget* w, const char* name, const char* fallback);

}  // namespace bg::gui
