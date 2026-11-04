// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/common_types.h"

class QWidget;

namespace ToolMods {

class ToolModManager {
public:
    static void ShowDialog(QWidget* parent, u64 program_id);
};

} // namespace ToolMods
