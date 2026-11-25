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

#ifndef OS_PROCESSGPU_H
#define OS_PROCESSGPU_H

#include "process.h"

#include <QElapsedTimer>
#include <QHash>
#include <QList>
#include <QStringList>
#include <QString>
#include <QVector>

#include <memory>

namespace OS
{
    /// Counters of one DRM engine, summed over the clients of one process.
    struct DrmEngineCounter
    {
        quint64  BusyNs      { 0 };      ///< drm-engine-<name>: nanoseconds the engine was busy.
        quint64  Cycles      { 0 };      ///< drm-cycles-<name> (xe style).
        quint64  TotalCycles { 0 };      ///< drm-total-cycles-<name> (xe style).
        unsigned Capacity    { 1 };      ///< drm-engine-capacity-<name>: number of identical instances.
        bool     HasCycles   { false };  ///< True when the driver reports cycles instead of nanoseconds.
    };
    using DrmEngineMap = QHash<QString, DrmEngineCounter>;

    /// One DRM client (one open file description of /dev/dri/* or /dev/accel/*).
    struct DrmClient
    {
        QString      Driver;     ///< drm-driver (amdgpu, i915, xe, msm, ...), may be empty.
        QString      Pdev;       ///< drm-pdev (PCI address), may be empty.
        QString      ClientId;   ///< drm-client-id, empty when the driver does not report one.
        DrmEngineMap Engines;    ///< Engine counters, empty when the driver reports none.
    };

    /// Pure functions behind the DRM fdinfo source, split out so they can be unit tested
    /// with fdinfo samples captured from real drivers (tests/fixtures/drm).
    namespace ProcessGpuDrm
    {
        /// Parse the content of a /proc/PID/fdinfo/N file. Returns false when it is not a DRM fdinfo.
        bool ParseFdInfo(const QByteArray &content, DrmClient &out);

        /// Add the engines of a client to the per-process totals (keys are "<pdev>/<engine>").
        void MergeClient(DrmEngineMap &totals, const DrmClient &client);

        /// Utilization of the busiest engine (0..1) between two samples, or a negative number
        /// when nothing could be measured (no common engine, counter went backwards, no time elapsed).
        /// This is also what the Windows Task Manager shows per process: the busiest engine.
        double BusiestEngineFraction(const DrmEngineMap &prev, const DrmEngineMap &cur, qint64 elapsed_ns);
    }

    /// What the sampler found on this system, to explain empty GPU columns.
    struct ProcessGpuDiagnostics
    {
        bool        DrmPresent { false };       ///< /dev/dri or /dev/accel exists.
        bool        NvmlReady { false };        ///< libnvidia-ml loaded and at least one device found.
        int         NvmlDevices { 0 };
        QStringList DrmDrivers;                 ///< "driver: engine-ns|cycles|no engine counters".
        int         Measured { 0 };             ///< Processes with a measured value in the last pass.
        int         Denied { 0 };               ///< Processes whose GPU clients could not be inspected.

        /// Single-line human readable description, also used for the log.
        QString Summary() const;
    };

    /// Per-process GPU utilization, independent of the system-wide GPU backends.
    ///
    /// Sources, combined per process with max():
    ///  - DRM fdinfo (/proc/PID/fdinfo/N of fds open on /dev/dri/* or /dev/accel/*):
    ///    drm-engine-<name> (ns), drm-engine-capacity-<name>, and the xe style
    ///    drm-cycles-<name> / drm-total-cycles-<name>. Works for amdgpu, i915, xe,
    ///    msm, panfrost, v3d, nouveau, ivpu and every other driver following
    ///    Documentation/gpu/drm-usage-stats.rst (Linux 5.19+ and later).
    ///  - NVML (dlopen of libnvidia-ml.so.1), for the proprietary NVIDIA driver
    ///    that exposes no DRM engine counters.
    ///
    /// Not thread safe: use one instance from a single thread.
    class ProcessGpuSampler
    {
        public:
            ProcessGpuSampler();
            ~ProcessGpuSampler();
            ProcessGpuSampler(const ProcessGpuSampler &) = delete;
            ProcessGpuSampler &operator=(const ProcessGpuSampler &) = delete;

            /// Fills GpuPercent / GpuAvailable / GpuPermissionDenied of every process.
            void Sample(QList<Process> &processes);

            /// Forget all previous samples (call when GPU metrics get disabled).
            void Reset();

            /// Sources found and processes measured during the last Sample().
            ProcessGpuDiagnostics Diagnostics() const;

        private:
            class Nvml;

            using EngineCounter = DrmEngineCounter;
            using EngineMap = DrmEngineMap;

            struct PidState
            {
                quint64      StartTimeTicks { 0 };
                QVector<int> DrmFds;
                int          LastScan { 0 };
                int          LastSeen { 0 };
                bool         Denied   { false };
                qint64       PrevNs   { 0 };
                EngineMap    Prev;
            };

            bool readDrmEngines(pid_t pid, const QVector<int> &fds, QVector<int> &valid_fds, EngineMap &out);
            static bool listDrmFds(pid_t pid, QVector<int> &fds, bool &denied);
            static bool hasDrmSupport();

            QHash<pid_t, PidState>  m_pids;
            QElapsedTimer           m_clock;
            int                     m_pass { 0 };
            bool                    m_drmPresent { false };
            QHash<QString, QString> m_driverKinds;   ///< drm-driver -> how it reports usage.
            int                     m_measured { 0 };
            int                     m_denied { 0 };
            QString                 m_lastSources;   ///< Last logged sources summary.
            bool                    m_drmEnginesSeen { false }; ///< Some process exposed DRM engine counters.
            std::unique_ptr<Nvml>   m_nvml;
    };
}

#endif // OS_PROCESSGPU_H
