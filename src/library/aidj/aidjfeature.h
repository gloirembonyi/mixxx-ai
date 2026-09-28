#pragma once

#include "library/libraryfeature.h"
#include "preferences/usersettings.h"
#include "util/parented_ptr.h"

class TreeItemModel;

namespace mixxx {
namespace aidj {

/// Sidebar entry "AI DJ": next track suggestions, sets and playlists
class AiDjFeature final : public LibraryFeature {
    Q_OBJECT
  public:
    AiDjFeature(Library* pLibrary, UserSettingsPointer pConfig);

    QVariant title() override;
    void bindLibraryWidget(WLibrary* pLibraryWidget, KeyboardEventFilter* pKeyboard) override;
    TreeItemModel* sidebarModel() const override;

  public slots:
    void activate() override;

  private:
    parented_ptr<TreeItemModel> m_pSidebarModel;
};

/// Sidebar entry "Free Music Finder": search and download legal free music
class MusicFinderFeature final : public LibraryFeature {
    Q_OBJECT
  public:
    MusicFinderFeature(Library* pLibrary, UserSettingsPointer pConfig);

    QVariant title() override;
    void bindLibraryWidget(WLibrary* pLibraryWidget, KeyboardEventFilter* pKeyboard) override;
    TreeItemModel* sidebarModel() const override;

  public slots:
    void activate() override;

  private:
    parented_ptr<TreeItemModel> m_pSidebarModel;
};

} // namespace aidj
} // namespace mixxx
