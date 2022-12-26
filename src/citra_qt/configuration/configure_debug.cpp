// Copyright 2016 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <QDesktopServices>
#include <QMessageBox>
#include <QUrl>
#include <QMessageBox>
#include "citra_qt/configuration/configure_debug.h"
#include "citra_qt/debugger/console.h"
#include "citra_qt/uisettings.h"
#include "common/file_util.h"
#include "common/logging/log.h"
#include "common/settings.h"
#include "core/core.h"
#include "qcheckbox.h"
#include "ui_configure_debug.h"
#include "video_core/renderer_vulkan/vk_instance.h"

ConfigureDebug::ConfigureDebug(QWidget* parent)
    : QWidget(parent), ui(std::make_unique<Ui::ConfigureDebug>()) {
    ui->setupUi(this);
    SetConfiguration();

    connect(ui->open_log_button, &QPushButton::clicked, []() {
        QString path = QString::fromStdString(FileUtil::GetUserPath(FileUtil::UserPath::LogDir));
        QDesktopServices::openUrl(QUrl::fromLocalFile(path));
    });

    connect(ui->toggle_renderer_debug, &QCheckBox::clicked, this, [this](bool checked) {
        if (checked && Settings::values.graphics_api.GetValue() == Settings::GraphicsAPI::Vulkan) {
            try {
                Vulkan::Instance debug_inst{true};
            } catch (vk::LayerNotPresentError&) {
                ui->toggle_renderer_debug->toggle();
                QMessageBox::warning(this, tr("Validation layer not available"),
                                     tr("Unable to enable debug renderer because the layer "
                                        "<strong>VK_LAYER_KHRONOS_validation</strong> is missing. "
                                        "Please install the Vulkan SDK or the appropriate package "
                                        "of your distribution"));
            }
        }
    });

    connect(ui->toggle_dump_command_buffers, &QCheckBox::clicked, this, [this](bool checked) {
        if (checked && Settings::values.graphics_api.GetValue() == Settings::GraphicsAPI::Vulkan) {
            try {
                Vulkan::Instance debug_inst{false, true};
            } catch (vk::LayerNotPresentError&) {
                ui->toggle_dump_command_buffers->toggle();
                QMessageBox::warning(this, tr("Command buffer dumping not available"),
                                     tr("Unable to enable command buffer dumping because the layer "
                                        "<strong>VK_LAYER_LUNARG_api_dump</strong> is missing. "
                                        "Please install the Vulkan SDK or the appropriate package "
                                        "of your distribution"));
            }
        }
    });

    const bool is_powered_on = Core::System::GetInstance().IsPoweredOn();
    ui->toggle_cpu_jit->setEnabled(!is_powered_on);
    ui->toggle_renderer_debug->setEnabled(!is_powered_on);
    ui->toggle_dump_command_buffers->setEnabled(!is_powered_on);
}

ConfigureDebug::~ConfigureDebug() = default;

void ConfigureDebug::SetConfiguration() {
    ui->toggle_gdbstub->setChecked(Settings::values.use_gdbstub.GetValue());
    ui->gdbport_spinbox->setEnabled(Settings::values.use_gdbstub.GetValue());
    ui->gdbport_spinbox->setValue(Settings::values.gdbstub_port.GetValue());
    ui->toggle_console->setEnabled(!Core::System::GetInstance().IsPoweredOn());
    ui->toggle_console->setChecked(UISettings::values.show_console.GetValue());
    ui->log_filter_edit->setText(QString::fromStdString(Settings::values.log_filter.GetValue()));
    ui->toggle_cpu_jit->setChecked(Settings::values.use_cpu_jit.GetValue());
    ui->toggle_renderer_debug->setChecked(Settings::values.renderer_debug.GetValue());
    ui->toggle_dump_command_buffers->setChecked(Settings::values.dump_command_buffers.GetValue());
}

void ConfigureDebug::ApplyConfiguration() {
    Settings::values.use_gdbstub = ui->toggle_gdbstub->isChecked();
    Settings::values.gdbstub_port = ui->gdbport_spinbox->value();
    UISettings::values.show_console = ui->toggle_console->isChecked();
    Settings::values.log_filter = ui->log_filter_edit->text().toStdString();
    Debugger::ToggleConsole();
    Log::Filter filter;
    filter.ParseFilterString(Settings::values.log_filter.GetValue());
    Log::SetGlobalFilter(filter);
    Settings::values.use_cpu_jit = ui->toggle_cpu_jit->isChecked();
    Settings::values.renderer_debug = ui->toggle_renderer_debug->isChecked();
    Settings::values.dump_command_buffers = ui->toggle_dump_command_buffers->isChecked();
}

void ConfigureDebug::RetranslateUI() {
    ui->retranslateUi(this);
}
