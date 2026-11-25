/*
 * Tux Manager - Linux system monitor
 * Copyright (C) 2026 Petr Bena <petr@bena.rocks>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef OS_PROCESSHEATMAP_H
#define OS_PROCESSHEATMAP_H

#include "process.h"
#include "../colorscheme.h"
#include "../configuration.h"
#include "../metrics.h"
#include "../system/memory.h"

#include <QColor>
#include <QVariant>
#include <QtGlobal>
#include <cmath>

namespace OS
{
    /// Shared by the flat and tree process models: background color of a
    /// usage cell, like the Windows Task Manager heatmap.
    namespace ProcessHeatmap
    {
        /// I/O rate that gives a fully saturated cell.
        constexpr double IoSaturationBps = 100.0 * 1024.0 * 1024.0;

        /// @param fraction usage in range 0..1; empty QVariant (no color) when disabled or idle.
        /// The square root lifts small values so light usage is still visible.
        inline QVariant Background(double fraction)
        {
            if (!CFG->ProcessHeatmap || fraction <= 0.0)
                return {};
            QColor color = ColorScheme::GetCurrent()->ProcessHeatmapColor;
            color.setAlphaF(color.alphaF() * std::sqrt(qMin(fraction, 1.0)));
            return color;
        }

        inline QVariant Cpu(const Process &proc)
        {
            return Background(proc.CPUPercent / 100.0);
        }

        inline QVariant Memory(const Process &proc)
        {
            const qint64 total_kb = Metrics::GetMemory()->MemTotalKb();
            return total_kb > 0 ? Background(static_cast<double>(proc.VMRssKb) / total_kb) : QVariant();
        }

        inline QVariant IoRate(const Process &proc, double bps)
        {
            return proc.IORatesAvailable ? Background(bps / IoSaturationBps) : QVariant();
        }
    }
}

#endif // OS_PROCESSHEATMAP_H
