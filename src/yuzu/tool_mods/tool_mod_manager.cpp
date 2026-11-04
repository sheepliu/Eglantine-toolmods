// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "tool_mod_manager.h"

#include <algorithm>
#include <filesystem>
#include <memory>
#include <system_error>
#include <utility>

#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QStandardPaths>
#include <QStringList>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "common/fs/path_util.h"

namespace ToolMods {
namespace {

constexpr int ManifestLimit = 64 * 1024;
constexpr int HostApiVersion = 1;

QString TitleIdString(u64 program_id) {
    return QString::number(program_id, 16).rightJustified(16, QLatin1Char('0')).toUpper();
}

struct Package {
    QString id;
    QString name;
    QString version;
    QString catalog;
    QString runtime;
    QString entrypoint;
    QStringList entrypoint_args;
    QStringList on_open_args;
    QStringList capabilities;
    bool enabled{};
};

bool IsValidId(const QString& id) {
    if (id.isEmpty() || id.size() > 80) {
        return false;
    }
    for (const QChar ch : id) {
        if (!(ch >= QLatin1Char('a') && ch <= QLatin1Char('z')) &&
            !(ch >= QLatin1Char('A') && ch <= QLatin1Char('Z')) &&
            !(ch >= QLatin1Char('0') && ch <= QLatin1Char('9')) && ch != QLatin1Char('.') &&
            ch != QLatin1Char('_') && ch != QLatin1Char('-')) {
            return false;
        }
    }
    return true;
}

QStringList ReadCapabilities(const QJsonValue& value, bool* valid) {
    QStringList capabilities;
    if (!value.isArray()) {
        *valid = false;
        return capabilities;
    }

    for (const QJsonValue& entry : value.toArray()) {
        if (!entry.isString() || entry.toString().isEmpty() || entry.toString().size() > 80) {
            *valid = false;
            return {};
        }
        capabilities.append(entry.toString());
    }
    capabilities.removeDuplicates();
    return capabilities;
}

QList<Package> DiscoverPackages(u64 program_id, QSettings& settings) {
    const QString title_id = TitleIdString(program_id);
    const std::filesystem::path root = Common::FS::GetEdenPath(Common::FS::EdenPath::ConfigDir) /
                                       "tool_mods" / title_id.toStdString();
    QList<Package> packages;
    std::error_code ec;
    if (!std::filesystem::is_directory(root, ec)) {
        return packages;
    }

    const QString settings_group = QStringLiteral("games/%1").arg(title_id);
    settings.beginGroup(settings_group);
    for (std::filesystem::directory_iterator it(root, ec), end; !ec && it != end;
         it.increment(ec)) {
        const auto package_status = it->symlink_status(ec);
        if (ec || std::filesystem::is_symlink(package_status) ||
            !std::filesystem::is_directory(package_status)) {
            ec.clear();
            continue;
        }
        const QString directory_id = QString::fromStdString(it->path().filename().string());
        if (!IsValidId(directory_id)) {
            continue;
        }

        const std::filesystem::path manifest_path = it->path() / "manifest.json";
        const auto manifest_status = std::filesystem::symlink_status(manifest_path, ec);
        if (ec || std::filesystem::is_symlink(manifest_status) ||
            !std::filesystem::is_regular_file(manifest_status)) {
            ec.clear();
            continue;
        }
        const auto file_size = std::filesystem::file_size(manifest_path, ec);
        if (ec || file_size == 0 || file_size > ManifestLimit) {
            ec.clear();
            continue;
        }

        QFile file(QString::fromStdString(Common::FS::PathToUTF8String(manifest_path)));
        if (!file.open(QIODevice::ReadOnly) || file.size() > ManifestLimit) {
            continue;
        }
        QJsonParseError parse_error{};
        const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parse_error);
        if (parse_error.error != QJsonParseError::NoError || !document.isObject()) {
            continue;
        }

        const QJsonObject manifest = document.object();
        const QString id = manifest.value(QStringLiteral("id")).toString();
        const QString name = manifest.value(QStringLiteral("name")).toString();
        const QString version = manifest.value(QStringLiteral("version")).toString();
        const QString catalog = manifest.value(QStringLiteral("catalog")).toString();
        const QString runtime = manifest.value(QStringLiteral("runtime")).toString();
        const QString entrypoint = manifest.value(QStringLiteral("entrypoint")).toString();
        QStringList entrypoint_args;
        const QJsonValue args_value = manifest.value(QStringLiteral("entrypoint_args"));
        // entrypoint_args is optional for catalog-only packages. Once a package
        // declares an entrypoint, its argument template must be an array.
        bool valid_entrypoint_args = args_value.isUndefined() || args_value.isArray();
        if (valid_entrypoint_args) {
            for (const QJsonValue& arg : args_value.toArray()) {
                if (!arg.isString() || arg.toString().size() > 512) {
                    valid_entrypoint_args = false;
                    break;
                }
                entrypoint_args.append(arg.toString());
            }
        }
        QStringList on_open_args;
        const QJsonValue on_open_value = manifest.value(QStringLiteral("on_open_args"));
        bool valid_on_open_args = on_open_value.isUndefined() || on_open_value.isArray();
        if (valid_on_open_args) {
            for (const QJsonValue& arg : on_open_value.toArray()) {
                if (!arg.isString() || arg.toString().size() > 512) {
                    valid_on_open_args = false;
                    break;
                }
                on_open_args.append(arg.toString());
            }
        }
        bool valid_capabilities = true;
        QStringList capabilities =
            ReadCapabilities(manifest.value(QStringLiteral("capabilities")), &valid_capabilities);
        if (manifest.value(QStringLiteral("manifest_version")).toInt() != 1 ||
            manifest.value(QStringLiteral("api_version")).toInt() != HostApiVersion ||
            !IsValidId(id) || id != directory_id || name.isEmpty() || name.size() > 120 ||
            version.isEmpty() || version.size() > 40 || !valid_capabilities ||
            (!catalog.isEmpty() &&
             (catalog.contains(QLatin1Char('/')) || catalog.contains(QLatin1Char('\\')) ||
              catalog == QStringLiteral(".") || catalog == QStringLiteral("..")))) {
            continue;
        }
        if ((!entrypoint.isEmpty() &&
             (entrypoint.contains(QLatin1Char('/')) ||
              entrypoint.contains(QLatin1Char('\\')) || entrypoint == QStringLiteral(".") ||
              entrypoint == QStringLiteral("..") || runtime.isEmpty() ||
              runtime.size() > 260)) ||
            !valid_entrypoint_args || !valid_on_open_args) {
            continue;
        }
        if (!entrypoint.isEmpty() && args_value.isUndefined()) {
            continue;
        }

        packages.append(Package{
            .id = id,
            .name = name,
            .version = version,
            .catalog = catalog,
            .runtime = runtime,
            .entrypoint = entrypoint,
            .entrypoint_args = std::move(entrypoint_args),
            .on_open_args = std::move(on_open_args),
            .capabilities = std::move(capabilities),
            .enabled = settings.value(id, false).toBool(),
        });
    }
    settings.endGroup();
    return packages;
}

} // namespace

void ToolModManager::ShowDialog(QWidget* parent, u64 program_id) {
    const QString title_id = TitleIdString(program_id);
    const std::filesystem::path settings_path =
        Common::FS::GetEdenPath(Common::FS::EdenPath::ConfigDir) / "tool_mods.ini";
    QSettings settings(QString::fromStdString(Common::FS::PathToUTF8String(settings_path)),
                       QSettings::IniFormat);
    QList<Package> packages = DiscoverPackages(program_id, settings);

    QDialog dialog(parent);
    dialog.setWindowTitle(QObject::tr("Runtime Tool Mods - %1").arg(title_id));
    dialog.resize(720, 420);
    auto* layout = new QVBoxLayout(&dialog);
    auto* description =
        new QLabel(QObject::tr("Packages are discovered from the per-title tool_mods "
                               "directory. The host validates each manifest and launches a "
                               "package entrypoint only when its package action is requested."),
                   &dialog);
    description->setWordWrap(true);
    layout->addWidget(description);

    auto* list = new QTreeWidget(&dialog);
    list->setColumnCount(5);
    list->setHeaderLabels({QObject::tr("Enabled"), QObject::tr("Name"), QObject::tr("Version"),
                           QObject::tr("Capabilities"), QObject::tr("Actions")});
    list->setRootIsDecorated(false);
    list->setUniformRowHeights(true);
    for (const Package& package : packages) {
        auto* row = new QTreeWidgetItem(list);
        row->setFlags(row->flags() | Qt::ItemIsUserCheckable);
        row->setCheckState(0, package.enabled ? Qt::Checked : Qt::Unchecked);
        row->setText(1, package.name);
        row->setText(2, package.version);
        row->setText(3, package.capabilities.join(QStringLiteral(", ")));
        row->setData(0, Qt::UserRole, package.id);
        row->setToolTip(1, package.id);
    }
    list->setColumnWidth(0, 85);
    list->setColumnWidth(1, 220);
    list->setColumnWidth(2, 100);
    list->setColumnWidth(4, 150);
    layout->addWidget(list);

    for (int i = 0; i < list->topLevelItemCount(); ++i) {
        QTreeWidgetItem* row = list->topLevelItem(i);
        const QString package_id = row->data(0, Qt::UserRole).toString();
        const auto package =
            std::find_if(packages.cbegin(), packages.cend(),
                         [&package_id](const Package& item) { return item.id == package_id; });
        if (package == packages.cend() || package->catalog.isEmpty() ||
            !package->capabilities.contains(QStringLiteral("inventory.add"))) {
            continue;
        }

        auto* open_catalog = new QPushButton(QObject::tr("Open Item Catalog..."), &dialog);
        open_catalog->setEnabled(package->enabled);
        list->setItemWidget(row, 4, open_catalog);
        QObject::connect(
            open_catalog, &QPushButton::clicked, &dialog,
            [&dialog, parent, title_id, package_id, catalog = package->catalog,
             runtime = package->runtime, entrypoint = package->entrypoint,
             entrypoint_args = package->entrypoint_args] {
                const std::filesystem::path package_dir =
                    Common::FS::GetEdenPath(Common::FS::EdenPath::ConfigDir) / "tool_mods" /
                    title_id.toStdString() / package_id.toStdString();
                const auto path = package_dir / catalog.toStdString();
                std::error_code ec;
                const auto status = std::filesystem::symlink_status(path, ec);
                if (ec || std::filesystem::is_symlink(status) ||
                    !std::filesystem::is_regular_file(status) ||
                    std::filesystem::file_size(path, ec) > 4 * 1024 * 1024 || ec) {
                    QMessageBox::warning(parent, QObject::tr("Item Catalog"),
                                         QObject::tr("The package catalog is missing "
                                                     "or invalid."));
                    return;
                }

                QFile file(QString::fromStdString(Common::FS::PathToUTF8String(path)));
                if (!file.open(QIODevice::ReadOnly)) {
                    QMessageBox::warning(parent, QObject::tr("Item Catalog"),
                                         QObject::tr("The package catalog could not "
                                                     "be opened."));
                    return;
                }
                QJsonParseError parse_error{};
                const QJsonDocument document =
                    QJsonDocument::fromJson(file.readAll(), &parse_error);
                if (parse_error.error != QJsonParseError::NoError || !document.isArray()) {
                    QMessageBox::warning(parent, QObject::tr("Item Catalog"),
                                         QObject::tr("The package catalog format is "
                                                     "invalid."));
                    return;
                }

                struct CatalogItem {
                    QString iid;
                    QString name_jp;
                    QString name_zh_tw;
                    QString name_zh_cn;
                };
                QList<CatalogItem> items;
                for (const QJsonValue& value : document.array()) {
                    if (!value.isObject()) {
                        continue;
                    }
                    const QJsonObject item = value.toObject();
                    CatalogItem entry{item.value(QStringLiteral("iid")).toString(),
                                      item.value(QStringLiteral("name_jp")).toString(),
                                      item.value(QStringLiteral("name_zh_tw")).toString(),
                                      item.value(QStringLiteral("name_zh_cn")).toString()};
                    if (entry.iid.startsWith(QStringLiteral("IID_")) && !entry.name_jp.isEmpty() &&
                        !entry.name_zh_tw.isEmpty() && !entry.name_zh_cn.isEmpty()) {
                        items.append(std::move(entry));
                    }
                }
                if (items.isEmpty()) {
                    QMessageBox::warning(parent, QObject::tr("Item Catalog"),
                                         QObject::tr("No valid item entries were "
                                                     "found."));
                    return;
                }

                auto* item_dialog = new QDialog(&dialog);
                item_dialog->setAttribute(Qt::WA_DeleteOnClose);
                item_dialog->setModal(false);
                item_dialog->setWindowModality(Qt::NonModal);
                item_dialog->setWindowTitle(QObject::tr("Add Item - %1").arg(title_id));
                item_dialog->resize(1040, 520);
                // An in-flight guest GDB request must not be orphaned by closing
                // the catalog window while the game is paused.
                item_dialog->setWindowFlag(Qt::WindowCloseButtonHint, false);
                auto* item_layout = new QVBoxLayout(item_dialog);
                auto* search = new QLineEdit(item_dialog);
                search->setPlaceholderText(QObject::tr("Search IID or item name"));
                item_layout->addWidget(search);
                auto* item_list = new QTreeWidget(item_dialog);
                item_list->setColumnCount(4);
                item_list->setHeaderLabels({QObject::tr("IID"), QObject::tr("Japanese"),
                                            QObject::tr("Traditional Chinese"),
                                            QObject::tr("Simplified Chinese")});
                item_list->setRootIsDecorated(false);
                item_list->setUniformRowHeights(true);
                for (const CatalogItem& entry : items) {
                    auto* item = new QTreeWidgetItem(item_list);
                    item->setText(0, entry.iid);
                    item->setText(1, entry.name_jp);
                    item->setText(2, entry.name_zh_tw);
                    item->setText(3, entry.name_zh_cn);
                }
                item_list->setColumnWidth(0, 250);
                item_list->setColumnWidth(1, 210);
                item_list->setColumnWidth(2, 210);
                item_layout->addWidget(item_list);
                QObject::connect(
                    search, &QLineEdit::textChanged, item_list, [item_list](const QString& query) {
                        for (int row = 0; row < item_list->topLevelItemCount(); ++row) {
                            QTreeWidgetItem* item = item_list->topLevelItem(row);
                            const bool match = item->text(0).contains(query, Qt::CaseInsensitive) ||
                                               item->text(1).contains(query, Qt::CaseInsensitive) ||
                                               item->text(2).contains(query, Qt::CaseInsensitive) ||
                                               item->text(3).contains(query, Qt::CaseInsensitive);
                            item->setHidden(!match);
                        }
                    });
                auto* bottom = new QHBoxLayout();
                auto* quantity = new QSpinBox(item_dialog);
                quantity->setRange(1, 999);
                quantity->setValue(1);
                 bottom->addWidget(new QLabel(QObject::tr("Quantity"), item_dialog));
                bottom->addWidget(quantity);
                bottom->addStretch();
                auto* grant = new QPushButton(QObject::tr("Add to Convoy"), item_dialog);
                const bool has_entrypoint = !runtime.isEmpty() && !entrypoint.isEmpty();
                grant->setEnabled(has_entrypoint);
                if (!has_entrypoint) {
                    grant->setToolTip(QObject::tr("This package has no runtime entrypoint."));
                }
                bottom->addWidget(grant);
                item_layout->addLayout(bottom);
                auto* grant_status = new QLabel(
                    has_entrypoint
                        ? QObject::tr("Select an item, choose a quantity, then add it. The "
                                      "package may wait for one in-game inventory event.")
                        : QObject::tr("Catalog ready. This package does not expose an add "
                                      "entrypoint."),
                    item_dialog);
                grant_status->setWordWrap(true);
                item_layout->addWidget(grant_status);
                auto* process = new QProcess(item_dialog);
                process->setProcessChannelMode(QProcess::MergedChannels);
                auto* process_log = new QFile(
                    QString::fromStdString(Common::FS::PathToUTF8String(package_dir /
                                                                          "runtime.log")),
                    item_dialog);
                const bool log_opened =
                    process_log->open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text);
                if (log_opened && process_log->isOpen()) {
                    process_log->write("\n--- entrypoint start ---\n");
                    process_log->flush();
                }
                auto process_running = std::make_shared<bool>(false);
                QObject::connect(process, &QProcess::readyReadStandardOutput, item_dialog,
                                 [process, process_log, grant_status] {
                                     const QByteArray data = process->readAllStandardOutput();
                                     if (process_log->isOpen() && !data.isEmpty()) {
                                         process_log->write(data);
                                         process_log->flush();
                                     }
                                     const QString output = QString::fromLocal8Bit(data).trimmed();
                                     if (!output.isEmpty()) {
                                         grant_status->setText(output.right(2048));
                                     }
                                 });
                QObject::connect(
                    process,
                    static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished),
                     item_dialog,
                     [process, process_log, grant, grant_status,
                      process_running](int exit_code, QProcess::ExitStatus) {
                         *process_running = false;
                        grant->setEnabled(true);
                        const QByteArray data = process->readAllStandardOutput();
                        if (process_log->isOpen() && !data.isEmpty()) {
                            process_log->write(data);
                            process_log->flush();
                        }
                        const QString output = QString::fromLocal8Bit(data).trimmed();
                        if (exit_code == 0) {
                            grant_status->setText(output.isEmpty()
                                                      ? QObject::tr("Item request completed.")
                                                      : output.right(2048));
                        } else {
                            grant_status->setText(
                                QObject::tr("Item request failed (exit %1). %2")
                                    .arg(exit_code)
                                    .arg(output.right(1800)));
                        }
                    });
                QObject::connect(process, &QProcess::errorOccurred, item_dialog,
                                  [process_log, grant, grant_status,
                                   process_running](QProcess::ProcessError) {
                                      *process_running = false;
                                     grant->setEnabled(true);
                                     if (process_log->isOpen()) {
                                         process_log->write("\n--- entrypoint error ---\n");
                                         process_log->flush();
                                     }
                                     grant_status->setText(
                                         QObject::tr("Could not start the package entrypoint."));
                                 });
                QObject::connect(
                    grant, &QPushButton::clicked, item_dialog,
                    [item_list, quantity, process, grant, grant_status,
                     process_running, package_dir, title_id, runtime, entrypoint,
                     entrypoint_args] {
                                      if (*process_running) {
                                          return;
                                      }
                                     const QTreeWidgetItem* selected = item_list->currentItem();
                                     if (selected == nullptr) {
                                         grant_status->setText(
                                             QObject::tr("Select an item before adding it."));
                                         return;
                                     }
                                     const QString iid = selected->text(0);
                                     const QString package_dir_string = QString::fromStdString(
                                         Common::FS::PathToUTF8String(package_dir));
                                     const QString entrypoint_path = QString::fromStdString(
                                         Common::FS::PathToUTF8String(package_dir /
                                                                       entrypoint.toStdString()));
                                     std::error_code entrypoint_ec;
                                     const auto entrypoint_status =
                                         std::filesystem::symlink_status(package_dir /
                                                                              entrypoint.toStdString(),
                                                                          entrypoint_ec);
                                     if (entrypoint_ec ||
                                         std::filesystem::is_symlink(entrypoint_status) ||
                                         !std::filesystem::is_regular_file(entrypoint_status)) {
                                         grant_status->setText(QObject::tr(
                                             "The package entrypoint is missing or invalid."));
                                         return;
                                     }

                                     QStringList args;
                                     const QString quantity_text = QString::number(quantity->value());
                                     for (QString arg : entrypoint_args) {
                                         arg.replace(QStringLiteral("{entrypoint}"), entrypoint_path);
                                         arg.replace(QStringLiteral("{package_dir}"),
                                                     package_dir_string);
                                         arg.replace(QStringLiteral("{title_id}"), title_id);
                                         arg.replace(QStringLiteral("{iid}"), iid);
                                         arg.replace(QStringLiteral("{quantity}"), quantity_text);
                                         args.append(std::move(arg));
                                     }
                                     process->setProgram(runtime);
                                     process->setArguments(args);
                                     process->setWorkingDirectory(package_dir_string);
                                      *process_running = true;
                                      grant->setEnabled(false);
                                      grant_status->setText(QObject::tr("Starting package entrypoint..."));
                                      process->start();
                                  });
                auto* close = new QDialogButtonBox(QDialogButtonBox::Close, item_dialog);
                 QObject::connect(close, &QDialogButtonBox::rejected, item_dialog,
                                  [item_dialog, grant_status, process_running] {
                                      if (*process_running) {
                                          grant_status->setText(QObject::tr(
                                              "An item request is still running; wait for it to "
                                              "finish before closing this window."));
                                          return;
                                      }
                                      item_dialog->close();
                                  });
                item_layout->addWidget(close);
                item_dialog->show();
                item_dialog->raise();
                item_dialog->activateWindow();
             });
    }

    if (packages.isEmpty()) {
        auto* empty = new QLabel(QObject::tr("No compatible packages found."), &dialog);
        layout->insertWidget(1, empty);
    }

    auto* buttons =
        new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);

    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    settings.beginGroup(QStringLiteral("games/%1").arg(title_id));
    for (int i = 0; i < list->topLevelItemCount(); ++i) {
        const QTreeWidgetItem* row = list->topLevelItem(i);
        settings.setValue(row->data(0, Qt::UserRole).toString(), row->checkState(0) == Qt::Checked);
    }
    settings.endGroup();
    settings.sync();
}

} // namespace ToolMods
