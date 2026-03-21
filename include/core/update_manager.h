#pragma once
/**
 * @file update_manager.h
 * @brief Over-The-Air (OTA) update manager
 *
 * Asynchronously checks GitHub Releases for new versions on startup.
 * If found, downloads the installer in the background and prompts
 * the user to restart and apply the update.
 */

#include <QObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QString>
#include <QFile>
#include <memory>

namespace vision {

class UpdateManager : public QObject {
    Q_OBJECT

public:
    explicit UpdateManager(QObject* parent = nullptr);
    ~UpdateManager() override = default;

    /// Set the GitHub repository (e.g. "username/VISION-AI")
    void setRepository(const QString& repo);

    /// Set the current application version (e.g. "3.0.0")
    void setCurrentVersion(const QString& version);

    /// Begin checking for updates (call once on startup)
    void checkForUpdates();

    /// Get download progress (0-100)
    int downloadProgress() const { return download_progress_; }

    /// Whether an update is available
    bool isUpdateAvailable() const { return update_available_; }

    /// Get the latest version string
    QString latestVersion() const { return latest_version_; }

    /// Get changelog / release notes
    QString releaseNotes() const { return release_notes_; }

signals:
    /// Emitted when a new version is detected
    void updateAvailable(const QString& version, const QString& notes);

    /// Emitted during download progress (0-100)
    void downloadProgressChanged(int percent);

    /// Emitted when the installer is downloaded and ready
    void updateReady(const QString& installer_path);

    /// Emitted on any error
    void updateError(const QString& error);

private slots:
    void onCheckFinished(QNetworkReply* reply);
    void onDownloadFinished(QNetworkReply* reply);
    void onDownloadProgress(qint64 received, qint64 total);

private:
    QNetworkAccessManager* network_ = nullptr;
    QString repository_;
    QString current_version_;
    QString latest_version_;
    QString release_notes_;
    QString download_url_;
    QString download_path_;
    int download_progress_ = 0;
    bool update_available_ = false;

    /// Compare semver strings (returns true if `remote` > `local`)
    static bool isNewerVersion(const QString& local, const QString& remote);
};

} // namespace vision
