#include "library/aidj/dlgmusicfinder.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QStandardPaths>
#include <QTableWidget>
#include <QVBoxLayout>

#include "control/controlobject.h"
#include "library/aidj/aidjwidgets.h"
#include "library/dao/playlistdao.h"
#include "library/library.h"
#include "library/trackcollection.h"
#include "library/trackcollectionmanager.h"
#include "mixer/playermanager.h"
#include "moc_dlgmusicfinder.cpp"
#include "track/track.h"
#include "track/trackref.h"

namespace mixxx {
namespace aidj {

namespace {

const QString kConfigGroup = QStringLiteral("[MusicFinder]");
const ConfigKey kFolderKey(kConfigGroup, QStringLiteral("DownloadFolder"));
const ConfigKey kPaidGigsKey(kConfigGroup, QStringLiteral("PaidGigsOnly"));
const ConfigKey kLosslessKey(kConfigGroup, QStringLiteral("PreferLossless"));
const ConfigKey kSourceKey(kConfigGroup, QStringLiteral("Source"));
const QString kPreviewGroup = QStringLiteral("[PreviewDeck1]");

const QColor kLicenseOkColor(0x3c, 0xc8, 0x5a);
const QColor kLicenseLimitedColor(0xf0, 0x8a, 0x24);
const QColor kDownloadBarColor(0x5b, 0x9b, 0xd5);

enum Column {
    kColumnTitle,
    kColumnArtist,
    kColumnLength,
    kColumnBpm,
    kColumnFormat,
    kColumnSize,
    kColumnLicense,
    kColumnSource,
    kColumnStatus,
    kColumnCount,
};

QTableWidgetItem* readOnlyItem(const QString& text) {
    auto* pItem = new QTableWidgetItem(text);
    pItem->setFlags(pItem->flags() & ~Qt::ItemIsEditable);
    return pItem;
}

QString formatDuration(double seconds) {
    if (seconds <= 0.0) {
        return QString();
    }
    const int total = qRound(seconds);
    return QStringLiteral("%1:%2").arg(total / 60).arg(total % 60, 2, 10, QChar('0'));
}

QString formatSize(qint64 bytes) {
    if (bytes <= 0) {
        return QString();
    }
    return QString::number(bytes / (1024.0 * 1024.0), 'f', 1) + QStringLiteral(" MB");
}

} // namespace

DlgMusicFinder::DlgMusicFinder(QWidget* pParent,
        UserSettingsPointer pConfig,
        Library* pLibrary,
        KeyboardEventFilter* pKeyboard)
        : QWidget(pParent),
          m_pConfig(pConfig),
          m_pLibrary(pLibrary),
          m_pSearch(new MusicSearch(this)),
          m_pDownloader(new MusicDownloader(this)),
          m_nextTag(0) {
    Q_UNUSED(pKeyboard);
    auto* pLayout = new QVBoxLayout(this);
    pLayout->setContentsMargins(6, 6, 6, 6);
    pLayout->setSpacing(6);

    // Card: title, search field and options
    auto* pCard = new QFrame(this);
    setRole(pCard, Role::Card);
    auto* pCardLayout = new QVBoxLayout(pCard);
    pCardLayout->setContentsMargins(10, 6, 10, 8);
    pCardLayout->setSpacing(6);

    auto* pTitleRow = new QHBoxLayout();
    pTitleRow->addWidget(makeLabel(tr("FREE MUSIC FINDER"), Role::Section, pCard));
    auto* pIntro = makeLabel(
            tr("Free, legal music by independent artists, downloaded straight "
               "into your library while the decks keep playing. Check the "
               "license before a paid gig and credit the artists."),
            Role::Muted,
            pCard);
    pIntro->setWordWrap(true);
    pTitleRow->addWidget(pIntro, 1);
    pCardLayout->addLayout(pTitleRow);

    auto* pSearchRow = new QHBoxLayout();
    pSearchRow->setSpacing(6);
    m_pQuery = new QLineEdit(pCard);
    m_pQuery->setPlaceholderText(tr("Genre, artist or title, e.g. \"deep house\" or \"afrobeat\""));
    m_pQuery->setClearButtonEnabled(true);
    pSearchRow->addWidget(m_pQuery, 1);
    m_pSource = new QComboBox(pCard);
    for (MusicSource source :
            {MusicSource::All, MusicSource::InternetArchive, MusicSource::CcMixter}) {
        m_pSource->addItem(musicSourceName(source), static_cast<int>(source));
    }
    m_pSource->setCurrentIndex(std::max(0,
            m_pSource->findData(m_pConfig->getValue(
                    kSourceKey, static_cast<int>(MusicSource::All)))));
    m_pSource->setToolTip(tr(
            "Internet Archive netlabels often have lossless "
            "FLAC, ccMixter has BPM information"));
    pSearchRow->addWidget(m_pSource);
    auto* pSearchButton = new QPushButton(tr("Search"), pCard);
    setRole(pSearchButton, Role::Primary);
    pSearchButton->setDefault(true);
    pSearchRow->addWidget(pSearchButton);
    pCardLayout->addLayout(pSearchRow);

    auto* pOptionsRow = new QHBoxLayout();
    pOptionsRow->setSpacing(10);
    m_pPaidGigsOnly = new QCheckBox(tr("Only licenses allowed at paid gigs"), pCard);
    m_pPaidGigsOnly->setToolTip(tr("Hides non-commercial (NC) licenses"));
    m_pPaidGigsOnly->setChecked(m_pConfig->getValue(kPaidGigsKey, true));
    pOptionsRow->addWidget(m_pPaidGigsOnly);
    m_pPreferLossless = new QCheckBox(tr("Prefer lossless (FLAC)"), pCard);
    m_pPreferLossless->setChecked(m_pConfig->getValue(kLosslessKey, true));
    pOptionsRow->addWidget(m_pPreferLossless);
    pOptionsRow->addStretch();
    m_pFolder = makeLabel(QString(), Role::Muted, pCard);
    m_pFolder->setTextFormat(Qt::PlainText);
    pOptionsRow->addWidget(m_pFolder);
    auto* pFolderButton = new QPushButton(tr("Change folder…"), pCard);
    pOptionsRow->addWidget(pFolderButton);
    pCardLayout->addLayout(pOptionsRow);
    pLayout->addWidget(pCard);

    m_pTable = new QTableWidget(0, kColumnCount, this);
    m_pTable->setHorizontalHeaderLabels({tr("Title"),
            tr("Artist"),
            tr("Length"),
            tr("BPM"),
            tr("Format"),
            tr("Size"),
            tr("License"),
            tr("Source"),
            tr("Status")});
    m_pTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_pTable->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_pTable->verticalHeader()->hide();
    m_pTable->verticalHeader()->setDefaultSectionSize(24);
    m_pTable->horizontalHeader()->setStretchLastSection(true);
    m_pTable->horizontalHeader()->setHighlightSections(false);
    m_pTable->setAlternatingRowColors(true);
    m_pTable->setShowGrid(false);
    m_pTable->setWordWrap(false);
    m_pTable->setItemDelegateForColumn(
            kColumnStatus, new ScoreBarDelegate(m_pTable, kDownloadBarColor));
    m_pTable->setToolTip(tr(
            "Double-click a track to download it and preview it "
            "in the headphones"));
    pLayout->addWidget(m_pTable, 1);

    m_pLicense = makeLabel(QString(), Role::Tip, this);
    m_pLicense->setWordWrap(true);
    m_pLicense->setTextFormat(Qt::PlainText);
    m_pLicense->hide();
    pLayout->addWidget(m_pLicense);

    auto* pActions = new QHBoxLayout();
    pActions->setSpacing(6);
    auto* pPreview = new QPushButton(tr("Download && preview"), this);
    setRole(pPreview, Role::Primary);
    auto* pDownload = new QPushButton(tr("Download"), this);
    auto* pLoad = new QPushButton(tr("Download && load to free deck"), this);
    auto* pQueue = new QPushButton(tr("Download && add to Auto DJ"), this);
    auto* pCredits = new QPushButton(tr("Copy credits"), this);
    pCredits->setToolTip(tr(
            "Copies the attribution of the selected tracks, e.g. "
            "for the description of a published mix"));
    auto* pSourcePage = new QPushButton(tr("Open source page"), this);
    pActions->addWidget(pPreview);
    pActions->addWidget(pDownload);
    pActions->addWidget(pLoad);
    pActions->addWidget(pQueue);
    pActions->addStretch();
    m_pDownloads = makeLabel(QString(), Role::Chip, this);
    m_pDownloads->hide();
    pActions->addWidget(m_pDownloads);
    pActions->addWidget(pCredits);
    pActions->addWidget(pSourcePage);
    pLayout->addLayout(pActions);

    m_pStatus = makeLabel(QString(), Role::Muted, this);
    m_pStatus->setWordWrap(true);
    m_pStatus->setTextFormat(Qt::PlainText);
    pLayout->addWidget(m_pStatus);

    connect(m_pQuery, &QLineEdit::returnPressed, this, &DlgMusicFinder::slotSearch);
    connect(pSearchButton, &QPushButton::clicked, this, &DlgMusicFinder::slotSearch);
    connect(m_pSource, &QComboBox::currentIndexChanged, this, [this]() {
        m_pConfig->setValue(kSourceKey, m_pSource->currentData().toInt());
    });
    connect(m_pPaidGigsOnly, &QCheckBox::toggled, this, [this](bool checked) {
        m_pConfig->setValue(kPaidGigsKey, checked);
        slotApplyFilter();
        // The Internet Archive filters licenses on the server
        if (!m_pQuery->text().trimmed().isEmpty() && !m_results.isEmpty()) {
            slotSearch();
        }
    });
    connect(m_pPreferLossless, &QCheckBox::toggled, this, [this](bool checked) {
        m_pConfig->setValue(kLosslessKey, checked);
    });
    connect(pFolderButton, &QPushButton::clicked, this, &DlgMusicFinder::slotChooseFolder);
    connect(pDownload, &QPushButton::clicked, this, [this]() {
        download(AfterDownload::AddToLibrary);
    });
    connect(pPreview, &QPushButton::clicked, this, [this]() {
        download(AfterDownload::Preview);
    });
    connect(pLoad, &QPushButton::clicked, this, [this]() {
        download(AfterDownload::LoadToFreeDeck);
    });
    connect(pQueue, &QPushButton::clicked, this, [this]() {
        download(AfterDownload::AddToAutoDj);
    });
    connect(pCredits, &QPushButton::clicked, this, &DlgMusicFinder::slotCopyCredits);
    connect(pSourcePage, &QPushButton::clicked, this, &DlgMusicFinder::slotOpenSourcePage);
    connect(m_pTable, &QTableWidget::itemSelectionChanged, this, [this]() {
        const QList<int> rows = selectedRows();
        if (rows.isEmpty()) {
            m_pLicense->hide();
            return;
        }
        const MusicSearchResult& result = m_results[rows.first()];
        m_pLicense->setText(tr("%1 - %2").arg(result.license.name, result.license.summary));
        m_pLicense->show();
    });
    connect(m_pTable, &QTableWidget::cellDoubleClicked, this, [this]() {
        download(AfterDownload::Preview);
    });

    connect(m_pSearch, &MusicSearch::resultsReady, this, &DlgMusicFinder::slotResultsReady);
    connect(m_pSearch, &MusicSearch::failed, this, &DlgMusicFinder::slotSearchFailed);
    connect(m_pDownloader, &MusicDownloader::progress, this, &DlgMusicFinder::slotDownloadProgress);
    connect(m_pDownloader, &MusicDownloader::finished, this, &DlgMusicFinder::slotDownloadFinished);
    connect(m_pDownloader, &MusicDownloader::failed, this, &DlgMusicFinder::slotDownloadFailed);

    m_pFolder->setText(tr("Saving to %1").arg(QDir::toNativeSeparators(downloadDirectory())));
    setStatus(tr("Type a genre, artist or title and press Enter."));
    applyPageStyle(this);
}

void DlgMusicFinder::onShow() {
    m_pQuery->setFocus();
}

bool DlgMusicFinder::hasFocus() const {
    return m_pTable->hasFocus() || m_pQuery->hasFocus();
}

void DlgMusicFinder::onSearch(const QString& text) {
    // The library search box searches the free music sources in this view
    m_pQuery->setText(text);
    slotSearch();
}

void DlgMusicFinder::setStatus(const QString& text) {
    m_pStatus->setText(text);
}

void DlgMusicFinder::setRowStatus(int row, const QString& text, std::optional<double> progress) {
    if (row < 0 || row >= m_pTable->rowCount()) {
        return;
    }
    QTableWidgetItem* pItem = m_pTable->item(row, kColumnStatus);
    if (!pItem) {
        return;
    }
    pItem->setText(text);
    pItem->setData(kBarValueRole, progress ? QVariant(*progress) : QVariant());
}

void DlgMusicFinder::updateDownloadCount() {
    const int pending = m_pDownloader->pendingCount();
    m_pDownloads->setVisible(pending > 0);
    m_pDownloads->setText(tr("%n download(s) running", "", pending));
}

QString DlgMusicFinder::downloadDirectory() const {
    const QString folder = m_pConfig->getValueString(kFolderKey);
    if (!folder.isEmpty()) {
        return folder;
    }
    return QDir(QStandardPaths::writableLocation(QStandardPaths::MusicLocation))
            .filePath(QStringLiteral("Mixxx Downloads"));
}

void DlgMusicFinder::slotChooseFolder() {
    const QString folder = QFileDialog::getExistingDirectory(
            this, tr("Folder for downloaded music"), downloadDirectory());
    if (folder.isEmpty()) {
        return;
    }
    m_pConfig->setValue(kFolderKey, folder);
    m_pFolder->setText(tr("Saving to %1").arg(QDir::toNativeSeparators(folder)));
}

void DlgMusicFinder::slotSearch() {
    const QString query = m_pQuery->text().trimmed();
    if (query.isEmpty()) {
        setStatus(tr("Type what you are looking for."));
        return;
    }
    const auto source = static_cast<MusicSource>(m_pSource->currentData().toInt());
    setStatus(source == MusicSource::All
                    ? tr("Searching all sources…")
                    : tr("Searching %1…").arg(musicSourceName(source)));
    m_pSearch->search(query,
            source,
            m_pPreferLossless->isChecked(),
            m_pPaidGigsOnly->isChecked());
}

void DlgMusicFinder::slotResultsReady(const QList<MusicSearchResult>& results) {
    // Running downloads continue, their rows are gone with the old results
    m_downloadRows.clear();
    m_results = results;
    // Tracks that can be played at paid gigs first
    std::stable_sort(m_results.begin(),
            m_results.end(),
            [](const MusicSearchResult& a, const MusicSearchResult& b) {
                return a.license.paidGigsOk && !b.license.paidGigsOk;
            });
    m_pTable->setSortingEnabled(false);
    m_pTable->clearContents();
    m_pTable->setRowCount(static_cast<int>(m_results.size()));
    for (int row = 0; row < m_results.size(); ++row) {
        const MusicSearchResult& result = m_results[row];
        auto* pTitle = readOnlyItem(result.title);
        QFont bold = pTitle->font();
        bold.setBold(true);
        pTitle->setFont(bold);
        if (!result.album.isEmpty()) {
            pTitle->setToolTip(tr("Album: %1").arg(result.album));
        }
        m_pTable->setItem(row, kColumnTitle, pTitle);
        m_pTable->setItem(row, kColumnArtist, readOnlyItem(result.artist));
        auto* pLength = readOnlyItem(formatDuration(result.durationSeconds));
        pLength->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        m_pTable->setItem(row, kColumnLength, pLength);
        auto* pBpm = readOnlyItem(result.bpm > 0 ? QString::number(result.bpm, 'f', 0) : QString());
        pBpm->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        m_pTable->setItem(row, kColumnBpm, pBpm);
        auto* pFormat = readOnlyItem(result.format);
        pFormat->setTextAlignment(Qt::AlignCenter);
        if (result.format == QStringLiteral("FLAC")) {
            pFormat->setForeground(kDownloadBarColor);
            pFormat->setFont(bold);
        }
        m_pTable->setItem(row, kColumnFormat, pFormat);
        auto* pSize = readOnlyItem(formatSize(result.sizeBytes));
        pSize->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        m_pTable->setItem(row, kColumnSize, pSize);
        auto* pLicense = readOnlyItem(result.license.name);
        pLicense->setToolTip(result.license.summary);
        pLicense->setForeground(
                result.license.paidGigsOk ? kLicenseOkColor : kLicenseLimitedColor);
        pLicense->setFont(bold);
        m_pTable->setItem(row, kColumnLicense, pLicense);
        m_pTable->setItem(row, kColumnSource, readOnlyItem(musicSourceName(result.source)));
        m_pTable->setItem(row, kColumnStatus, readOnlyItem(QString()));
    }
    m_pTable->resizeColumnsToContents();
    for (int column : {kColumnTitle, kColumnArtist}) {
        m_pTable->setColumnWidth(column, std::min(m_pTable->columnWidth(column), 320));
    }
    slotApplyFilter();
    int visible = 0;
    for (int row = 0; row < m_pTable->rowCount(); ++row) {
        if (!m_pTable->isRowHidden(row)) {
            ++visible;
        }
    }
    if (m_results.isEmpty()) {
        setStatus(tr("Nothing found. Try a genre or fewer words, or another source."));
    } else if (visible == 0) {
        setStatus(tr(
                "Found %1 tracks, but none with a license for paid gigs. "
                "Uncheck the license filter to see them.")
                        .arg(m_results.size()));
    } else {
        setStatus(tr(
                "Found %1 tracks. Double-click a track to download and "
                "preview it in the headphones.")
                        .arg(visible));
    }
}

void DlgMusicFinder::slotApplyFilter() {
    const bool paidOnly = m_pPaidGigsOnly->isChecked();
    for (int row = 0; row < m_results.size() && row < m_pTable->rowCount(); ++row) {
        m_pTable->setRowHidden(row, paidOnly && !m_results[row].license.paidGigsOk);
    }
}

void DlgMusicFinder::slotSearchFailed(const QString& message) {
    setStatus(message);
}

QList<int> DlgMusicFinder::selectedRows() const {
    QList<int> rows;
    const QList<QTableWidgetSelectionRange> ranges = m_pTable->selectedRanges();
    for (const auto& range : ranges) {
        for (int row = range.topRow(); row <= range.bottomRow(); ++row) {
            if (!m_pTable->isRowHidden(row) && row < m_results.size()) {
                rows << row;
            }
        }
    }
    std::sort(rows.begin(), rows.end());
    return rows;
}

void DlgMusicFinder::download(AfterDownload action) {
    const QList<int> rows = selectedRows();
    if (rows.isEmpty()) {
        setStatus(tr("Select one or more tracks first."));
        return;
    }
    int queued = 0;
    for (int row : rows) {
        const int tag = m_nextTag++;
        // Registered before enqueue(): a file that exists already finishes
        // right away, inside enqueue()
        m_downloadRows.insert(tag, row);
        m_downloadActions.insert(tag, action);
        setRowStatus(row, tr("Queued"), std::nullopt);
        if (!m_pDownloader->enqueue(m_results[row], downloadDirectory(), tag)) {
            // Already downloading
            m_downloadRows.remove(tag);
            m_downloadActions.remove(tag);
            continue;
        }
        ++queued;
        // Only the first track goes to the preview deck or a free deck
        if (action == AfterDownload::Preview || action == AfterDownload::LoadToFreeDeck) {
            action = AfterDownload::AddToLibrary;
        }
    }
    updateDownloadCount();
    if (queued > 0 && m_pDownloader->pendingCount() > 0) {
        setStatus(tr("Downloading %n track(s) in the background.", "", queued));
    }
}

void DlgMusicFinder::slotDownloadProgress(int tag, qint64 received, qint64 total) {
    const int row = m_downloadRows.value(tag, -1);
    if (total > 0) {
        const double fraction = static_cast<double>(received) / total;
        setRowStatus(row, QString::number(qRound(fraction * 100)) + QChar('%'), fraction);
    } else {
        setRowStatus(row, formatSize(received), std::nullopt);
    }
}

QString DlgMusicFinder::freeDeckGroup() const {
    const int numDecks = static_cast<int>(
            ControlObject::get(ConfigKey(QStringLiteral("[App]"), QStringLiteral("num_decks"))));
    for (int i = 0; i < numDecks; ++i) {
        const QString group = PlayerManager::groupForDeck(i);
        if (ControlObject::get(ConfigKey(group, QStringLiteral("play"))) == 0.0) {
            return group;
        }
    }
    return QString();
}

void DlgMusicFinder::slotDownloadFinished(
        int tag, const QString& filePath, const MusicSearchResult& result) {
    const int row = m_downloadRows.value(tag, -1);
    m_downloadRows.remove(tag);
    const AfterDownload action = m_downloadActions.value(tag, AfterDownload::AddToLibrary);
    m_downloadActions.remove(tag);
    updateDownloadCount();
    setRowStatus(row, tr("In library"), std::nullopt);
    if (row >= 0 && row < m_pTable->rowCount()) {
        if (QTableWidgetItem* pItem = m_pTable->item(row, kColumnStatus)) {
            pItem->setForeground(kLicenseOkColor);
        }
    }
    TrackPointer pTrack = m_pLibrary->trackCollectionManager()->getOrAddTrack(
            TrackRef::fromFilePath(filePath));
    if (!pTrack) {
        setStatus(tr("Downloaded %1, but it could not be added to the library.")
                        .arg(QDir::toNativeSeparators(filePath)));
        return;
    }
    // Keep the attribution with the track, CC licenses require credit
    if (pTrack->getComment().isEmpty()) {
        pTrack->setComment(tr("%1 - License: %2 - Source: %3")
                        .arg(result.license.name,
                                result.licenseUrl,
                                result.pageUrl.toString()));
    }
    switch (action) {
    case AfterDownload::Preview:
        emit loadTrackToPlayer(pTrack, kPreviewGroup, true);
        setStatus(tr("Previewing %1 in the headphones.").arg(result.title));
        break;
    case AfterDownload::LoadToFreeDeck: {
        const QString group = freeDeckGroup();
        if (group.isEmpty()) {
            setStatus(tr(
                    "Downloaded %1. All decks are playing, load it when a "
                    "deck is free.")
                            .arg(result.title));
        } else {
            emit loadTrackToPlayer(pTrack, group, false);
            setStatus(tr("Loaded %1 to %2.").arg(result.title, group));
        }
        break;
    }
    case AfterDownload::AddToAutoDj:
        m_pLibrary->trackCollectionManager()
                ->internalCollection()
                ->getPlaylistDAO()
                .addTracksToAutoDJQueue({pTrack->getId()}, PlaylistDAO::AutoDJSendLoc::BOTTOM);
        setStatus(tr("Added %1 to the Auto DJ queue.").arg(result.title));
        break;
    case AfterDownload::AddToLibrary:
    default:
        setStatus(tr(
                "Downloaded %1 into the library. Mixxx analyzes BPM and "
                "key when you load it.")
                        .arg(result.title));
        break;
    }
}

void DlgMusicFinder::slotDownloadFailed(int tag, const QString& message) {
    const int row = m_downloadRows.value(tag, -1);
    m_downloadRows.remove(tag);
    m_downloadActions.remove(tag);
    updateDownloadCount();
    setRowStatus(row, tr("Failed"), std::nullopt);
    if (row >= 0 && row < m_pTable->rowCount()) {
        if (QTableWidgetItem* pItem = m_pTable->item(row, kColumnStatus)) {
            pItem->setForeground(QColor(0xe0, 0x50, 0x3a));
            pItem->setToolTip(message);
        }
    }
    setStatus(message);
}

void DlgMusicFinder::slotCopyCredits() {
    const QList<int> rows = selectedRows();
    if (rows.isEmpty()) {
        setStatus(tr("Select the tracks to credit first."));
        return;
    }
    QStringList lines;
    for (int row : rows) {
        const MusicSearchResult& result = m_results[row];
        QString line = QStringLiteral("\"%1\" by %2").arg(result.title, result.artist);
        line += QStringLiteral(" – ") + result.license.name;
        if (!result.licenseUrl.isEmpty()) {
            line += QStringLiteral(" (") + result.licenseUrl + QChar(')');
        }
        line += QStringLiteral(" – ") + result.pageUrl.toString();
        lines << line;
    }
    QApplication::clipboard()->setText(lines.join(QChar('\n')));
    setStatus(tr("Copied the credits of %n track(s) to the clipboard.",
            "",
            static_cast<int>(rows.size())));
}

void DlgMusicFinder::slotOpenSourcePage() {
    const QList<int> rows = selectedRows();
    if (rows.isEmpty()) {
        return;
    }
    QDesktopServices::openUrl(m_results[rows.first()].pageUrl);
}

} // namespace aidj
} // namespace mixxx
