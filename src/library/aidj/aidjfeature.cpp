#include "library/aidj/aidjfeature.h"

#include "controllers/keyboard/keyboardeventfilter.h"
#include "library/aidj/dlgaidj.h"
#include "library/aidj/dlgmusicfinder.h"
#include "library/treeitemmodel.h"
#include "moc_aidjfeature.cpp"
#include "widget/wlibrary.h"

namespace mixxx {
namespace aidj {

namespace {

const QString kAiDjViewName = QStringLiteral("AiDj");
const QString kMusicFinderViewName = QStringLiteral("MusicFinder");

} // namespace

AiDjFeature::AiDjFeature(Library* pLibrary, UserSettingsPointer pConfig)
        : LibraryFeature(pLibrary, pConfig, QStringLiteral("aidj")),
          m_pSidebarModel(make_parented<TreeItemModel>(this)) {
}

QVariant AiDjFeature::title() {
    return tr("AI DJ");
}

TreeItemModel* AiDjFeature::sidebarModel() const {
    return m_pSidebarModel;
}

void AiDjFeature::bindLibraryWidget(WLibrary* pLibraryWidget, KeyboardEventFilter* pKeyboard) {
    // The view is deleted by the library widget
    auto* pView = new DlgAiDj(pLibraryWidget, m_pConfig, m_pLibrary, pKeyboard);
    pView->installEventFilter(pKeyboard);
    pLibraryWidget->registerView(kAiDjViewName, pView);
    connect(pView, &DlgAiDj::loadTrack, this, &AiDjFeature::loadTrack);
    connect(pView, &DlgAiDj::loadTrackToPlayer, this, &AiDjFeature::loadTrackToPlayer);
}

void AiDjFeature::activate() {
    emit switchToView(kAiDjViewName);
    emit disableSearch();
    emit enableCoverArtDisplay(false);
}

MusicFinderFeature::MusicFinderFeature(Library* pLibrary, UserSettingsPointer pConfig)
        : LibraryFeature(pLibrary, pConfig, QStringLiteral("musicfinder")),
          m_pSidebarModel(make_parented<TreeItemModel>(this)) {
}

QVariant MusicFinderFeature::title() {
    return tr("Free Music Finder");
}

TreeItemModel* MusicFinderFeature::sidebarModel() const {
    return m_pSidebarModel;
}

void MusicFinderFeature::bindLibraryWidget(
        WLibrary* pLibraryWidget, KeyboardEventFilter* pKeyboard) {
    auto* pView = new DlgMusicFinder(pLibraryWidget, m_pConfig, m_pLibrary, pKeyboard);
    pView->installEventFilter(pKeyboard);
    pLibraryWidget->registerView(kMusicFinderViewName, pView);
    connect(pView, &DlgMusicFinder::loadTrack, this, &MusicFinderFeature::loadTrack);
    connect(pView,
            &DlgMusicFinder::loadTrackToPlayer,
            this,
            &MusicFinderFeature::loadTrackToPlayer);
}

void MusicFinderFeature::activate() {
    emit switchToView(kMusicFinderViewName);
    emit disableSearch();
    emit enableCoverArtDisplay(false);
}

} // namespace aidj
} // namespace mixxx
