// Dialogs and shared actions: quick save, download groups, favorites, monitors, tools, about.
#pragma once

#include "core/download.hpp"
#include "core/profile.hpp"
#include "gui/widgets.hpp"

namespace bg::gui {

// Saves an image with the default (or favorites) filename and folder, reporting in the status bar
void quickSave(const ImagePtr& img, bool favorite, std::function<void(const SaveResults&)> done = nullptr);
// Reverse image search services: name -> URL template with {url}
std::vector<std::pair<std::string, std::string>> webServices();

bool editGroupDialog(GtkWindow* parent, DownloadQueryGroup& group, const std::string& title = "Download group");
bool editFavoriteDialog(GtkWindow* parent, Favorite& fav, bool isNew);
bool editMonitorDialog(GtkWindow* parent, Monitor& monitor, bool standalone, std::string* tags = nullptr);
void addFavorite(GtkWindow* parent, const std::string& tag);

void showAbout(GtkWindow* parent);
void md5DatabaseDialog(GtkWindow* parent);
void emptyFoldersDialog(GtkWindow* parent);
void tagLoaderDialog(GtkWindow* parent);
void importGrabberDialog(GtkWindow* parent);

}  // namespace bg::gui
