// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "tool_mod_manager.h"

#include <algorithm>
#include <filesystem>
#include <memory>
#include <system_error>
#include <utility>

#include <QCoreApplication>
#include <QCloseEvent>
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
#include <QPointer>
#include <QProcess>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QStackedWidget>
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

std::filesystem::path TitlePackageRoot(const QString& title_id) {
    return std::filesystem::u8path(QCoreApplication::applicationDirPath().toUtf8().constData()) /
           "tool_mods" / title_id.toStdString();
}

QString ResolveRuntimeProgram(const QString& runtime,
                              const std::filesystem::path& package_dir) {
    const bool looks_like_path = runtime.contains(QLatin1Char('/')) ||
                                 runtime.contains(QLatin1Char('\\')) ||
                                 runtime.startsWith(QLatin1Char('.'));
    if (!looks_like_path) {
        return runtime;
    }

    const auto resolved = (package_dir / std::filesystem::u8path(runtime.toUtf8().constData()))
                              .lexically_normal();
    return QString::fromStdString(Common::FS::PathToUTF8String(resolved));
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
    struct UiSpec {
        QString type;
        QString title;
        QString input_label;
        QString button_label;
        QString status_text;
        int minimum{1};
        int maximum{999};
        int default_value{1};
        bool valid{};
    } ui;
    bool enabled{};
};

class ToolModDialog final : public QDialog {
public:
    explicit ToolModDialog(QWidget* parent) : QDialog(parent) {}

    QPointer<QProcess> active_process;

protected:
    void closeEvent(QCloseEvent* event) override {
        if (WarnIfBusy()) {
            event->ignore();
            return;
        }
        QDialog::closeEvent(event);
    }

    void reject() override {
        if (!WarnIfBusy()) {
            QDialog::reject();
        }
    }

private:
    bool WarnIfBusy() {
        if (!active_process || active_process->state() == QProcess::NotRunning) {
            return false;
        }
        QMessageBox::information(this, QObject::tr("Item Request"),
                                 QObject::tr("An item request is still running; wait for it "
                                             "to finish before closing this window."));
        return true;
    }
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

Package::UiSpec ReadUiSpec(const QJsonValue& value, bool* valid) {
    Package::UiSpec ui;
    if (value.isUndefined() || value.isNull()) {
        return ui;
    }
    if (!value.isObject()) {
        *valid = false;
        return ui;
    }

    const QJsonObject object = value.toObject();
    ui.type = object.value(QStringLiteral("type")).toString();
    if (ui.type != QStringLiteral("numeric_action")) {
        *valid = false;
        return ui;
    }
    ui.title = object.value(QStringLiteral("title")).toString();
    ui.input_label = object.value(QStringLiteral("input_label")).toString();
    ui.button_label = object.value(QStringLiteral("button_label")).toString();
    ui.status_text = object.value(QStringLiteral("status_text")).toString();
    if (ui.title.isEmpty() || ui.input_label.isEmpty() || ui.button_label.isEmpty() ||
        ui.status_text.isEmpty() || ui.title.size() > 120 || ui.input_label.size() > 120 ||
        ui.button_label.size() > 120 || ui.status_text.size() > 500) {
        *valid = false;
        return ui;
    }

    const auto read_int = [&object](const QString& key, int fallback) {
        const QJsonValue number = object.value(key);
        return number.isUndefined() ? fallback : number.toInt(fallback);
    };
    ui.minimum = read_int(QStringLiteral("minimum"), 1);
    ui.maximum = read_int(QStringLiteral("maximum"), 50000);
    ui.default_value = read_int(QStringLiteral("default"), ui.minimum);
    if (ui.minimum < 1 || ui.maximum < ui.minimum || ui.maximum > 50000 ||
        ui.default_value < ui.minimum || ui.default_value > ui.maximum) {
        *valid = false;
        return ui;
    }
    ui.valid = true;
    return ui;
}

QList<Package> DiscoverPackages(u64 program_id, QSettings& settings) {
    const QString title_id = TitleIdString(program_id);
    const std::filesystem::path root = TitlePackageRoot(title_id);
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
        bool valid_ui = true;
        Package::UiSpec ui = ReadUiSpec(manifest.value(QStringLiteral("ui")), &valid_ui);
        if (manifest.value(QStringLiteral("manifest_version")).toInt() != 1 ||
            manifest.value(QStringLiteral("api_version")).toInt() != HostApiVersion ||
            !IsValidId(id) || id != directory_id || name.isEmpty() || name.size() > 120 ||
            version.isEmpty() || version.size() > 40 || !valid_capabilities || !valid_ui ||
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
            .ui = std::move(ui),
            .enabled = settings.value(id, false).toBool(),
        });
    }
    settings.endGroup();
    return packages;
}

} // namespace

void ToolModManager::ShowDialog(QWidget* parent, u64 program_id) {
    static QPointer<ToolModDialog> current_dialog;
    static u64 current_program_id = 0;
    if (current_dialog) {
        if (current_program_id == program_id || !current_dialog->close()) {
            current_dialog->show();
            current_dialog->raise();
            current_dialog->activateWindow();
            return;
        }
        current_dialog = nullptr;
    }

    const QString title_id = TitleIdString(program_id);
    const std::filesystem::path settings_path =
        Common::FS::GetEdenPath(Common::FS::EdenPath::ConfigDir) / "tool_mods.ini";
    auto* dialog = new ToolModDialog(parent);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowModality(Qt::NonModal);
    dialog->setWindowTitle(QObject::tr("Runtime Tool Mods - %1").arg(title_id));
    dialog->resize(720, 420);
    current_dialog = dialog;
    current_program_id = program_id;

    auto* settings = new QSettings(
        QString::fromStdString(Common::FS::PathToUTF8String(settings_path)),
        QSettings::IniFormat, dialog);
    QList<Package> packages = DiscoverPackages(program_id, *settings);
    auto* window_layout = new QVBoxLayout(dialog);
    window_layout->setContentsMargins(0, 0, 0, 0);
    auto* pages = new QStackedWidget(dialog);
    window_layout->addWidget(pages);
    auto* manager_page = new QWidget(pages);
    pages->addWidget(manager_page);
    auto* layout = new QVBoxLayout(manager_page);
    auto* description =
        new QLabel(QObject::tr("Packages are discovered from tool_mods/<title ID> beside the "
                               "emulator executable. The host validates each manifest and "
                               "launches a package entrypoint only when its package action "
                               "is requested."),
                   manager_page);
    description->setWordWrap(true);
    layout->addWidget(description);

    auto* list = new QTreeWidget(manager_page);
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
        if (package == packages.cend()) {
            continue;
        }

        if (package->ui.valid) {
            const auto ui = package->ui;
            auto* open_action = new QPushButton(ui.title + QStringLiteral("..."), manager_page);
            open_action->setEnabled(package->enabled);
            list->setItemWidget(row, 4, open_action);
            QObject::connect(
                open_action, &QPushButton::clicked, dialog,
                [dialog, pages, manager_page, title_id, package_id,
                 runtime = package->runtime, entrypoint = package->entrypoint,
                 entrypoint_args = package->entrypoint_args, ui] {
                    const std::filesystem::path package_dir =
                        TitlePackageRoot(title_id) / package_id.toStdString();
                    const QString package_dir_string = QString::fromStdString(
                        Common::FS::PathToUTF8String(package_dir));
                    const QString entrypoint_path = QString::fromStdString(
                        Common::FS::PathToUTF8String(package_dir / entrypoint.toStdString()));
                    const QString runtime_program = ResolveRuntimeProgram(runtime, package_dir);
                    std::error_code entrypoint_ec;
                    const auto entrypoint_status =
                        std::filesystem::symlink_status(package_dir / entrypoint.toStdString(),
                                                        entrypoint_ec);
                    if (runtime.isEmpty() || entrypoint.isEmpty() || entrypoint_ec ||
                        std::filesystem::is_symlink(entrypoint_status) ||
                        !std::filesystem::is_regular_file(entrypoint_status)) {
                        QMessageBox::warning(dialog, ui.title,
                                             QObject::tr("The package entrypoint is missing or invalid."));
                        return;
                    }

                    auto* action_page = new QWidget(pages);
                    pages->addWidget(action_page);
                    auto* action_layout = new QVBoxLayout(action_page);
                    auto* description = new QLabel(ui.status_text, action_page);
                    description->setWordWrap(true);
                    action_layout->addWidget(description);
                    auto* value_row = new QHBoxLayout();
                    value_row->addWidget(new QLabel(ui.input_label, action_page));
                    auto* value = new QSpinBox(action_page);
                    value->setRange(ui.minimum, ui.maximum);
                    value->setValue(ui.default_value);
                    value_row->addWidget(value);
                    value_row->addStretch();
                    action_layout->addLayout(value_row);
                    auto* run = new QPushButton(ui.button_label, action_page);
                    auto* cancel = new QPushButton(QObject::tr("Cancel Request"), action_page);
                    cancel->setEnabled(false);
                    auto* action_buttons = new QHBoxLayout();
                    action_buttons->addStretch();
                    action_buttons->addWidget(cancel);
                    action_buttons->addWidget(run);
                    action_layout->addLayout(action_buttons);
                    auto* status = new QLabel(action_page);
                    status->setWordWrap(true);
                    action_layout->addWidget(status);

                    auto* process = new QProcess(action_page);
                    const QString cancel_file = QString::fromStdString(
                        Common::FS::PathToUTF8String(package_dir / "cancel.request"));
                    process->setProcessChannelMode(QProcess::MergedChannels);
                    auto* process_log = new QFile(
                        QString::fromStdString(Common::FS::PathToUTF8String(package_dir /
                                                                            "runtime.log")),
                        action_page);
                    const bool log_opened =
                        process_log->open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text);
                    Q_UNUSED(log_opened);
                    auto process_running = std::make_shared<bool>(false);
                    QObject::connect(process, &QProcess::readyReadStandardOutput, action_page,
                                     [process, process_log, status] {
                                         const QByteArray data = process->readAllStandardOutput();
                                         if (process_log->isOpen() && !data.isEmpty()) {
                                             process_log->write(data);
                                             process_log->flush();
                                         }
                                         const QString output = QString::fromLocal8Bit(data).trimmed();
                                         if (!output.isEmpty()) {
                                             status->setText(output.right(2048));
                                         }
                                     });
                    QObject::connect(
                        process,
                        static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished),
                        action_page,
                        [dialog, process, process_log, run, cancel, status, process_running,
                         cancel_file](int exit_code,
                                                                              QProcess::ExitStatus) {
                            *process_running = false;
                            QFile::remove(cancel_file);
                            if (dialog->active_process == process) {
                                dialog->active_process = nullptr;
                            }
                            run->setEnabled(true);
                            cancel->setEnabled(false);
                            const QByteArray data = process->readAllStandardOutput();
                            if (process_log->isOpen() && !data.isEmpty()) {
                                process_log->write(data);
                                process_log->flush();
                            }
                            const QString output = QString::fromLocal8Bit(data).trimmed();
                            status->setText(exit_code == 0
                                                ? (output.isEmpty() ? QObject::tr("Action completed.")
                                                                    : output.right(2048))
                                                : QObject::tr("Action failed (exit %1). %2")
                                                      .arg(exit_code)
                                                      .arg(output.right(1800)));
                        });
                    QObject::connect(process, &QProcess::errorOccurred, action_page,
                                     [dialog, process, run, cancel, status,
                                      cancel_file,
                                      process_running](QProcess::ProcessError) {
                                         *process_running = false;
                                         QFile::remove(cancel_file);
                                         if (dialog->active_process == process) {
                                             dialog->active_process = nullptr;
                                         }
                                         run->setEnabled(true);
                                         cancel->setEnabled(false);
                                         status->setText(QObject::tr(
                                             "Could not start the package entrypoint."));
                                     });
                    QObject::connect(
                        run, &QPushButton::clicked, action_page,
                        [dialog, process, run, cancel, status, process_running, cancel_file,
                         package_dir_string,
                         title_id, runtime_program, entrypoint_path, entrypoint_args, value] {
                            if (*process_running) {
                                return;
                            }
                            QStringList args;
                            const QString value_text = QString::number(value->value());
                            for (QString arg : entrypoint_args) {
                                arg.replace(QStringLiteral("{entrypoint}"), entrypoint_path);
                                arg.replace(QStringLiteral("{package_dir}"), package_dir_string);
                                arg.replace(QStringLiteral("{title_id}"), title_id);
                                arg.replace(QStringLiteral("{value}"), value_text);
                                arg.replace(QStringLiteral("{quantity}"), value_text);
                                arg.replace(QStringLiteral("{cancel_file}"), cancel_file);
                                args.append(std::move(arg));
                            }
                            process->setProgram(runtime_program);
                            process->setArguments(args);
                            process->setWorkingDirectory(package_dir_string);
                            QFile::remove(cancel_file);
                            *process_running = true;
                            dialog->active_process = process;
                            run->setEnabled(false);
                            cancel->setEnabled(true);
                            status->setText(QObject::tr("Starting package action..."));
                            process->start();
                        });
                    QObject::connect(
                        cancel, &QPushButton::clicked, action_page,
                        [cancel, status, process_running, cancel_file] {
                            if (!*process_running) {
                                return;
                            }
                            cancel->setEnabled(false);
                            status->setText(QObject::tr("Cancelling package action..."));
                            QFile flag(cancel_file);
                            if (flag.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                                flag.write("cancel\n");
                                flag.close();
                            }
                        });
                    auto* back = new QPushButton(QObject::tr("Back to Mod List"), action_page);
                    QObject::connect(back, &QPushButton::clicked, action_page,
                                     [dialog, pages, manager_page, action_page, process,
                                      status, process_running, title_id] {
                                         if (*process_running ||
                                             process->state() != QProcess::NotRunning) {
                                             status->setText(QObject::tr(
                                                 "An action is still running; wait for it to finish before leaving this page."));
                                             return;
                                         }
                                         pages->setCurrentWidget(manager_page);
                                         dialog->setWindowTitle(
                                             QObject::tr("Runtime Tool Mods - %1").arg(title_id));
                                         pages->removeWidget(action_page);
                                         action_page->deleteLater();
                                     });
                    auto* footer = new QHBoxLayout();
                    footer->addStretch();
                    footer->addWidget(back);
                    action_layout->addLayout(footer);
                    dialog->setWindowTitle(ui.title + QStringLiteral(" - ") + title_id);
                    dialog->resize(620, 300);
                    pages->setCurrentWidget(action_page);
                });
            continue;
        }

        if (package->catalog.isEmpty() ||
            !package->capabilities.contains(QStringLiteral("inventory.add"))) {
            continue;
        }

        auto* open_catalog = new QPushButton(QObject::tr("Open Item Catalog..."), manager_page);
        open_catalog->setEnabled(package->enabled);
        list->setItemWidget(row, 4, open_catalog);
        QObject::connect(
            open_catalog, &QPushButton::clicked, dialog,
            [dialog, pages, manager_page, title_id, package_id, catalog = package->catalog,
             runtime = package->runtime, entrypoint = package->entrypoint,
             entrypoint_args = package->entrypoint_args] {
                const std::filesystem::path package_dir =
                    TitlePackageRoot(title_id) / package_id.toStdString();
                const auto path = package_dir / catalog.toStdString();
                std::error_code ec;
                const auto status = std::filesystem::symlink_status(path, ec);
                if (ec || std::filesystem::is_symlink(status) ||
                    !std::filesystem::is_regular_file(status) ||
                    std::filesystem::file_size(path, ec) > 4 * 1024 * 1024 || ec) {
                    QMessageBox::warning(dialog, QObject::tr("Item Catalog"),
                                         QObject::tr("The package catalog is missing "
                                                     "or invalid."));
                    return;
                }

                QFile file(QString::fromStdString(Common::FS::PathToUTF8String(path)));
                if (!file.open(QIODevice::ReadOnly)) {
                    QMessageBox::warning(dialog, QObject::tr("Item Catalog"),
                                         QObject::tr("The package catalog could not "
                                                     "be opened."));
                    return;
                }
                QJsonParseError parse_error{};
                const QJsonDocument document =
                    QJsonDocument::fromJson(file.readAll(), &parse_error);
                if (parse_error.error != QJsonParseError::NoError || !document.isArray()) {
                    QMessageBox::warning(dialog, QObject::tr("Item Catalog"),
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
                    QMessageBox::warning(dialog, QObject::tr("Item Catalog"),
                                         QObject::tr("No valid item entries were "
                                                     "found."));
                    return;
                }

                auto* item_dialog = new QWidget(pages);
                pages->addWidget(item_dialog);
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
                auto* cancel = new QPushButton(QObject::tr("Cancel Request"), item_dialog);
                cancel->setEnabled(false);
                const bool has_entrypoint = !runtime.isEmpty() && !entrypoint.isEmpty();
                grant->setEnabled(has_entrypoint);
                if (!has_entrypoint) {
                    grant->setToolTip(QObject::tr("This package has no runtime entrypoint."));
                }
                bottom->addWidget(cancel);
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
                const QString cancel_file = QString::fromStdString(
                    Common::FS::PathToUTF8String(package_dir / "cancel.request"));
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
                     [dialog, process, process_log, grant, cancel, grant_status, cancel_file,
                      process_running](int exit_code, QProcess::ExitStatus) {
                         *process_running = false;
                        QFile::remove(cancel_file);
                        if (dialog->active_process == process) {
                            dialog->active_process = nullptr;
                        }
                        grant->setEnabled(true);
                        cancel->setEnabled(false);
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
                                  [dialog, process, process_log, grant, cancel, grant_status,
                                   cancel_file,
                                   process_running](QProcess::ProcessError) {
                                      *process_running = process->state() != QProcess::NotRunning;
                                     if (!*process_running) {
                                         QFile::remove(cancel_file);
                                     }
                                     grant->setEnabled(!*process_running);
                                     cancel->setEnabled(*process_running);
                                     if (!*process_running && dialog->active_process == process) {
                                         dialog->active_process = nullptr;
                                     }
                                     if (process_log->isOpen()) {
                                         process_log->write("\n--- entrypoint error ---\n");
                                         process_log->flush();
                                     }
                                     grant_status->setText(
                                         QObject::tr("Could not start the package entrypoint."));
                                 });
                QObject::connect(
                    grant, &QPushButton::clicked, item_dialog,
                    [dialog, item_list, quantity, process, grant, cancel, grant_status,
                     cancel_file,
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
                                         arg.replace(QStringLiteral("{value}"), quantity_text);
                                         arg.replace(QStringLiteral("{cancel_file}"), cancel_file);
                                         args.append(std::move(arg));
                                     }
                                     process->setProgram(
                                         ResolveRuntimeProgram(runtime, package_dir));
                                     process->setArguments(args);
                                     process->setWorkingDirectory(package_dir_string);
                                      QFile::remove(cancel_file);
                                      *process_running = true;
                                      dialog->active_process = process;
                                      grant->setEnabled(false);
                                      cancel->setEnabled(true);
                                      grant_status->setText(QObject::tr("Starting package entrypoint..."));
                                      process->start();
                                  });
                QObject::connect(
                    cancel, &QPushButton::clicked, item_dialog,
                    [cancel, grant_status, process_running, cancel_file] {
                        if (!*process_running) {
                            return;
                        }
                        cancel->setEnabled(false);
                        grant_status->setText(QObject::tr("Cancelling item request..."));
                        QFile flag(cancel_file);
                        if (flag.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                            flag.write("cancel\n");
                            flag.close();
                        }
                    });
                auto* back = new QPushButton(QObject::tr("Back to Mod List"), item_dialog);
                QObject::connect(back, &QPushButton::clicked, item_dialog,
                                  [dialog, pages, manager_page, item_dialog, process,
                                   grant_status, process_running, title_id] {
                                      if (*process_running ||
                                          process->state() != QProcess::NotRunning) {
                                          grant_status->setText(QObject::tr(
                                              "An item request is still running; wait for it to "
                                              "finish before leaving this page."));
                                          return;
                                      }
                                      pages->setCurrentWidget(manager_page);
                                      dialog->setWindowTitle(
                                          QObject::tr("Runtime Tool Mods - %1").arg(title_id));
                                      pages->removeWidget(item_dialog);
                                      item_dialog->deleteLater();
                                  });
                auto* footer = new QHBoxLayout();
                footer->addStretch();
                footer->addWidget(back);
                item_layout->addLayout(footer);
                dialog->setWindowTitle(QObject::tr("Add Item - %1").arg(title_id));
                dialog->resize(1040, 520);
                pages->setCurrentWidget(item_dialog);
             });
    }

    if (packages.isEmpty()) {
        auto* empty = new QLabel(QObject::tr("No compatible packages found."), manager_page);
        layout->insertWidget(1, empty);
    }

    QObject::connect(list, &QTreeWidget::itemChanged, dialog,
                     [settings, title_id, list](QTreeWidgetItem* row, int column) {
                         if (column != 0) {
                             return;
                         }
                         const bool enabled = row->checkState(0) == Qt::Checked;
                         settings->beginGroup(QStringLiteral("games/%1").arg(title_id));
                         settings->setValue(row->data(0, Qt::UserRole).toString(), enabled);
                         settings->endGroup();
                         settings->sync();
                         if (QWidget* action = list->itemWidget(row, 4)) {
                             action->setEnabled(enabled);
                         }
                     });
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, manager_page);
    QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::close);
    layout->addWidget(buttons);
    dialog->show();
}

} // namespace ToolMods
