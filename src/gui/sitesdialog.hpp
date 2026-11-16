// Site selection, site settings and adding sites.
#pragma once

#include <memory>

#include "core/site.hpp"
#include "gui/widgets.hpp"

namespace bg::gui {

bool chooseSitesDialog(GtkWindow* parent, StringList& sites);
void siteSettingsDialog(GtkWindow* parent, Site* site);
std::shared_ptr<Site> addSiteDialog(GtkWindow* parent);

}  // namespace bg::gui
