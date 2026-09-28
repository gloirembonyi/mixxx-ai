#pragma once

#include <QHash>
#include <QList>
#include <QWidget>
#include <optional>

#include "library/aidj/musicsources.h"
#include "library/libraryview.h"
#include "preferences/usersettings.h"
#include "track/track_decl.h"

class Library;
class KeyboardEventFilter;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;

namespace mixxx {
namespace aidj {

/// Library view to search legal, free music (Internet Archive netlabels and
/// ccMixter), download it into the library in the background and load it
/// into a deck, the preview deck or the Auto DJ queue.
class DlgMusicFinder : public QWidget, public virtual LibraryView {
    Q_OBJECT
  public:
    DlgMusicFinder(QWidget* pParent,
            UserSettingsPointer pConfig,
            Library* pLibrary,
            KeyboardEventFilter* pKeyboard);

    void onShow() override;
    bool hasFocus() const override;
    void onSearch(const QString& text) override;

  signals:
    void loadTrack(TrackPointer pTrack);
    void loadTrackToPlayer(TrackPointer pTrack, const QString& group, bool play);

  private slots:
    void slotSearch();
    void slotResultsReady(const QList<MusicSearchResult>& results);
    void slotSearchFailed(const QString& message);
    void slotDownloadProgress(int tag, qint64 received, qint64 total);
    void slotDownloadFinished(int tag, const QString& filePath, const MusicSearchResult& result);
    void slotDownloadFailed(int tag, const QString& message);
    void slotOpenSourcePage();
    void slotCopyCredits();
    void slotChooseFolder();
    void slotApplyFilter();

  private:
    enum class AfterDownload {
        AddToLibrary,
        Preview,
        LoadToFreeDeck,
        AddToAutoDj,
    };

    QList<int> selectedRows() const;
    void download(AfterDownload action);
    QString downloadDirectory() const;
    QString freeDeckGroup() const;
    void setStatus(const QString& text);
    void setRowStatus(int row, const QString& text, std::optional<double> progress);
    void updateDownloadCount();

    UserSettingsPointer m_pConfig;
    Library* m_pLibrary;
    MusicSearch* m_pSearch;
    MusicDownloader* m_pDownloader;

    QList<MusicSearchResult> m_results;
    // Row of each download by tag and what to do when it is finished
    QHash<int, int> m_downloadRows;
    QHash<int, AfterDownload> m_downloadActions;
    int m_nextTag;

    QLineEdit* m_pQuery;
    QComboBox* m_pSource;
    QCheckBox* m_pPaidGigsOnly;
    QCheckBox* m_pPreferLossless;
    QTableWidget* m_pTable;
    QLabel* m_pLicense;
    QLabel* m_pFolder;
    QLabel* m_pDownloads;
    QLabel* m_pStatus;
};

} // namespace aidj
} // namespace mixxx
