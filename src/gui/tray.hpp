// Optional tray icon (libayatana-appindicator): show/hide the window, quit.
#pragma once

namespace bg::gui {

bool trayAvailable();
void setupTray();
void removeTray();

}  // namespace bg::gui
