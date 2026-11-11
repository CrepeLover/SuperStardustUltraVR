// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <string_view>

namespace Common {

// Farpoint 1.00 releases that share the same SDK usage: CUSA04508 (EU) is the tested
// release; CUSA04179 (US) is the same game version under its own title ID.
constexpr bool IsFarpointSerial(std::string_view serial) {
    return serial == "CUSA04508" || serial == "CUSA04179";
}

} // namespace Common
