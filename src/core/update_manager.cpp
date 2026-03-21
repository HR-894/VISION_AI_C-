/**
 * @file update_manager.cpp
 * @brief OTA update manager implementation
 *
 * Uses QNetworkAccessManager to asynchronously check the GitHub Releases
 * API, compare semantic versions, and download the installer .exe.
 */

#include "update_manager.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QStandardPaths>
#include <QDir>
#include <QCoreApplication>

#ifdef VISION_HAS_SPDLOG
#include <spdlog/spdlog.h>
#define LOG_INFO(...) spdlog::info(__VA_ARGS__)
#define LOG_WARN(...) spdlog::warn(__VA_ARGS__)
#define LOG_ERROR(...) spdlog::error(__VA_ARGS__)
#else
#define LOG_INFO(...) (void)0
#define LOG_WARN(...) (void)0
#define LOG_ERROR(...) (void)0
#endif

namespace vision {

UpdateManager::UpdateManager(QObject* parent)
    : QObject(parent) {
    network_ = new QNetworkAccessManager(this);
}

void UpdateManager::setRepository(const QString& repo) {
    repository_ = repo;
}

void UpdateManager::setCurrentVersion(const QString& version) {
    current_version_ = version;
}

void UpdateManager::checkForUpdates() {
    if (repository_.isEmpty() || current_version_.isEmpty()) {
        LOG_WARN("UpdateManager: repository or version not set");
        return;
    }

    QString api_url = QString("https://api.github.com/repos/%1/releases/latest")
                          .arg(repository_);

    LOG_INFO("Checking for updates: {}", api_url.toStdString());

    QNetworkRequest request(api_url);
    request.setHeader(QNetworkRequest::UserAgentHeader, "VISION-AI-Updater/3.0");
    request.setRawHeader("Accept", "application/vnd.github.v3+json");

    QNetworkReply* reply = network_->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        onCheckFinished(reply);
    });
}

void UpdateManager::onCheckFinished(QNetworkReply* reply) {
    reply->deleteLater();

    if (reply->error() != QNetworkReply::NoError) {
        LOG_WARN("Update check failed: {}", reply->errorString().toStdString());
        emit updateError("Failed to check for updates: " + reply->errorString());
        return;
    }

    QByteArray data = reply->readAll();
    QJsonDocument doc = QJsonDocument::fromJson(data);
    if (!doc.isObject()) {
        emit updateError("Invalid response from GitHub API");
        return;
    }

    QJsonObject release = doc.object();
    QString tag_name = release.value("tag_name").toString();
    QString body = release.value("body").toString();

    // Strip leading 'v' from tag (e.g. "v3.1.0" → "3.1.0")
    QString remote_version = tag_name;
    if (remote_version.startsWith('v') || remote_version.startsWith('V')) {
        remote_version = remote_version.mid(1);
    }

    LOG_INFO("Current version: {} | Latest: {}",
             current_version_.toStdString(), remote_version.toStdString());

    if (!isNewerVersion(current_version_, remote_version)) {
        LOG_INFO("Application is up to date");
        return;
    }

    // Find the installer asset (.exe or .zip)
    QJsonArray assets = release.value("assets").toArray();
    for (const auto& asset_val : assets) {
        QJsonObject asset = asset_val.toObject();
        QString name = asset.value("name").toString().toLower();
        if (name.endsWith(".exe") || name.endsWith(".zip")) {
            download_url_ = asset.value("browser_download_url").toString();
            break;
        }
    }

    if (download_url_.isEmpty()) {
        LOG_WARN("No installer asset found in release {}", tag_name.toStdString());
        emit updateError("Update found but no installer asset available");
        return;
    }

    latest_version_ = remote_version;
    release_notes_ = body;
    update_available_ = true;

    LOG_INFO("Update available: {} → {}", current_version_.toStdString(),
             latest_version_.toStdString());

    emit updateAvailable(latest_version_, release_notes_);

    // Auto-download the installer to temp
    QString temp_dir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    QDir().mkpath(temp_dir);

    QString filename = QUrl(download_url_).fileName();
    download_path_ = temp_dir + "/" + filename;

    LOG_INFO("Downloading update to: {}", download_path_.toStdString());

    QNetworkRequest dl_request(download_url_);
    dl_request.setHeader(QNetworkRequest::UserAgentHeader, "VISION-AI-Updater/3.0");
    // GitHub redirects — follow them
    dl_request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                            QNetworkRequest::NoLessSafeRedirectPolicy);

    QNetworkReply* dl_reply = network_->get(dl_request);
    connect(dl_reply, &QNetworkReply::downloadProgress,
            this, &UpdateManager::onDownloadProgress);
    connect(dl_reply, &QNetworkReply::finished, this, [this, dl_reply]() {
        onDownloadFinished(dl_reply);
    });
}

void UpdateManager::onDownloadProgress(qint64 received, qint64 total) {
    if (total > 0) {
        download_progress_ = static_cast<int>((received * 100) / total);
        emit downloadProgressChanged(download_progress_);
    }
}

void UpdateManager::onDownloadFinished(QNetworkReply* reply) {
    reply->deleteLater();

    if (reply->error() != QNetworkReply::NoError) {
        LOG_ERROR("Update download failed: {}", reply->errorString().toStdString());
        emit updateError("Download failed: " + reply->errorString());
        return;
    }

    QFile file(download_path_);
    if (!file.open(QIODevice::WriteOnly)) {
        LOG_ERROR("Cannot write update file: {}", download_path_.toStdString());
        emit updateError("Cannot save update file");
        return;
    }

    file.write(reply->readAll());
    file.close();

    download_progress_ = 100;
    LOG_INFO("Update downloaded successfully: {}", download_path_.toStdString());
    emit updateReady(download_path_);
}

bool UpdateManager::isNewerVersion(const QString& local, const QString& remote) {
    auto parse = [](const QString& v) -> std::tuple<int, int, int> {
        QStringList parts = v.split('.');
        int major = parts.size() > 0 ? parts[0].toInt() : 0;
        int minor = parts.size() > 1 ? parts[1].toInt() : 0;
        int patch = parts.size() > 2 ? parts[2].toInt() : 0;
        return {major, minor, patch};
    };

    auto [lmaj, lmin, lpat] = parse(local);
    auto [rmaj, rmin, rpat] = parse(remote);

    if (rmaj != lmaj) return rmaj > lmaj;
    if (rmin != lmin) return rmin > lmin;
    return rpat > lpat;
}

} // namespace vision
