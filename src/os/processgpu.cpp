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

#include "processgpu.h"

#include "../logger.h"

#include <QByteArray>
#include <QDir>
#include <QSet>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>

#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>

using namespace OS;

namespace
{
    /// fd lists are re-read every this many passes (new GPU clients show up with that delay).
    constexpr int RescanEveryPasses = 5;
    constexpr int MaxFdInfoBytes = 16384;

    bool startsWith(const QByteArray &s, const char *prefix)
    {
        return s.startsWith(prefix);
    }

    quint64 firstNumber(const QByteArray &value)
    {
        return value.trimmed().split(' ').value(0).toULongLong();
    }
}

// ── NVML (proprietary NVIDIA driver) ─────────────────────────────────────────

class ProcessGpuSampler::Nvml
{
    private:
        struct UtilSample
        {
            unsigned int pid;
            unsigned long long timeStamp;
            unsigned int smUtil;
            unsigned int memUtil;
            unsigned int encUtil;
            unsigned int decUtil;
        };

    public:
        ~Nvml()
        {
            if (this->m_ready && this->m_shutdown)
                this->m_shutdown();
            if (this->m_lib)
                ::dlclose(this->m_lib);
        }

        bool IsReady() const { return this->m_ready; }
        int  DeviceCount() const { return static_cast<int>(this->m_devices.size()); }

        bool Ready()
        {
            if (!this->m_tried)
            {
                this->m_tried = true;
                this->load();
            }
            return this->m_ready;
        }

        /// Latest per-process utilization (0..100) of every NVIDIA device; max of SM, encoder and decoder.
        void Sample(QHash<pid_t, double> &out)
        {
            if (!this->Ready())
                return;

            for (int i = 0; i < this->m_devices.size(); ++i)
            {
                QHash<unsigned int, QPair<quint64, unsigned int>> latest;
                unsigned int count = 0;
                this->m_procUtil(this->m_devices[i], nullptr, &count, this->m_lastTs[i]);
                if (count == 0)
                    continue;

                QVector<UtilSample> buf(static_cast<int>(count) + 16);
                count = static_cast<unsigned int>(buf.size());
                if (this->m_procUtil(this->m_devices[i], buf.data(), &count, this->m_lastTs[i]) != 0)
                    continue;

                for (unsigned int s = 0; s < count; ++s)
                {
                    const UtilSample &sample = buf[static_cast<int>(s)];
                    this->m_lastTs[i] = std::max<quint64>(this->m_lastTs[i], sample.timeStamp);
                    const unsigned int util = std::max(sample.smUtil, std::max(sample.encUtil, sample.decUtil));
                    const auto it = latest.constFind(sample.pid);
                    if (it == latest.cend() || sample.timeStamp >= it.value().first)
                        latest.insert(sample.pid, { sample.timeStamp, util });
                }

                for (auto it = latest.cbegin(); it != latest.cend(); ++it)
                {
                    double &slot = out[static_cast<pid_t>(it.key())];
                    slot = std::max(slot, static_cast<double>(it.value().second));
                }
            }
        }

    private:
        using FnInit      = int (*)();
        using FnShutdown  = int (*)();
        using FnCount     = int (*)(unsigned int *);
        using FnHandle    = int (*)(unsigned int, void **);
        using FnProcUtil  = int (*)(void *, UtilSample *, unsigned int *, unsigned long long);

        void load()
        {
            this->m_lib = ::dlopen("libnvidia-ml.so.1", RTLD_LAZY | RTLD_LOCAL);
            if (!this->m_lib)
                this->m_lib = ::dlopen("libnvidia-ml.so", RTLD_LAZY | RTLD_LOCAL);
            if (!this->m_lib)
                return;

            auto init = reinterpret_cast<FnInit>(::dlsym(this->m_lib, "nvmlInit_v2"));
            this->m_shutdown = reinterpret_cast<FnShutdown>(::dlsym(this->m_lib, "nvmlShutdown"));
            auto count = reinterpret_cast<FnCount>(::dlsym(this->m_lib, "nvmlDeviceGetCount_v2"));
            auto handle = reinterpret_cast<FnHandle>(::dlsym(this->m_lib, "nvmlDeviceGetHandleByIndex_v2"));
            this->m_procUtil = reinterpret_cast<FnProcUtil>(::dlsym(this->m_lib, "nvmlDeviceGetProcessUtilization"));
            if (!init || !this->m_shutdown || !count || !handle || !this->m_procUtil)
                return;
            if (init() != 0)
                return;

            unsigned int n = 0;
            if (count(&n) == 0)
            {
                for (unsigned int i = 0; i < n; ++i)
                {
                    void *dev = nullptr;
                    if (handle(i, &dev) == 0 && dev)
                    {
                        this->m_devices.append(dev);
                        this->m_lastTs.append(0);
                    }
                }
            }

            this->m_ready = !this->m_devices.isEmpty();
            if (!this->m_ready)
                this->m_shutdown();
        }

        void *            m_lib { nullptr };
        FnShutdown        m_shutdown { nullptr };
        FnProcUtil        m_procUtil { nullptr };
        QVector<void *>   m_devices;
        QVector<quint64>  m_lastTs;
        bool              m_tried { false };
        bool              m_ready { false };
};

// ── DRM fdinfo pure functions ────────────────────────────────────────────────

bool ProcessGpuDrm::ParseFdInfo(const QByteArray &content, DrmClient &out)
{
    out = DrmClient();
    bool is_drm = false;
    QSet<QString> counted;   // engines that have a real counter, not just a capacity line

    const QList<QByteArray> lines = content.split('\n');
    for (const QByteArray &line : lines)
    {
        const int colon = line.indexOf(':');
        if (colon <= 0 || !startsWith(line, "drm-"))
            continue;
        is_drm = true;
        const QByteArray key = line.left(colon);
        const QByteArray value = line.mid(colon + 1).trimmed();

        if (key == "drm-driver")
            out.Driver = QString::fromLatin1(value);
        else if (key == "drm-pdev")
            out.Pdev = QString::fromLatin1(value);
        else if (key == "drm-client-id")
            out.ClientId = QString::fromLatin1(value);
        else if (startsWith(key, "drm-engine-capacity-"))
            out.Engines[QString::fromLatin1(key.mid(20))].Capacity = std::max<unsigned>(1, static_cast<unsigned>(firstNumber(value)));
        else if (startsWith(key, "drm-engine-"))
        {
            const QString name = QString::fromLatin1(key.mid(11));
            out.Engines[name].BusyNs = firstNumber(value);
            counted.insert(name);
        }
        else if (startsWith(key, "drm-total-cycles-"))
        {
            const QString name = QString::fromLatin1(key.mid(17));
            DrmEngineCounter &c = out.Engines[name];
            c.TotalCycles = firstNumber(value);
            c.HasCycles = true;
            counted.insert(name);
        }
        else if (startsWith(key, "drm-cycles-"))
        {
            // Cycles are only usable together with drm-total-cycles-X (xe). msm and panfrost
            // report drm-cycles-X next to drm-engine-X (ns) with no total: use the nanoseconds.
            out.Engines[QString::fromLatin1(key.mid(11))].Cycles = firstNumber(value);
        }
    }

    // "drm-engine-capacity-X" alone (no counter for X) does not make X an engine.
    for (auto it = out.Engines.begin(); it != out.Engines.end();)
    {
        if (!counted.contains(it.key()))
            it = out.Engines.erase(it);
        else
            ++it;
    }
    return is_drm;
}

void ProcessGpuDrm::MergeClient(DrmEngineMap &totals, const DrmClient &client)
{
    for (auto it = client.Engines.cbegin(); it != client.Engines.cend(); ++it)
    {
        DrmEngineCounter &sum = totals[client.Pdev + QLatin1Char('/') + it.key()];
        sum.BusyNs      += it.value().BusyNs;
        sum.Cycles      += it.value().Cycles;
        sum.TotalCycles += it.value().TotalCycles;
        sum.Capacity     = std::max(sum.Capacity, it.value().Capacity);
        sum.HasCycles    = sum.HasCycles || it.value().HasCycles;
    }
}

double ProcessGpuDrm::BusiestEngineFraction(const DrmEngineMap &prev, const DrmEngineMap &cur, qint64 elapsed_ns)
{
    double best = -1.0;
    for (auto it = cur.cbegin(); it != cur.cend(); ++it)
    {
        const auto prev_it = prev.constFind(it.key());
        if (prev_it == prev.cend())
            continue;
        const DrmEngineCounter &c = it.value();
        const DrmEngineCounter &p = prev_it.value();

        double fraction = -1.0;
        if (c.HasCycles)
        {
            // xe: ratio of the engine's cycles to the cycles that elapsed on the whole engine class.
            if (c.TotalCycles > p.TotalCycles && c.Cycles >= p.Cycles)
                fraction = static_cast<double>(c.Cycles - p.Cycles) / static_cast<double>(c.TotalCycles - p.TotalCycles);
        } else if (elapsed_ns > 0 && c.BusyNs >= p.BusyNs)
        {
            fraction = static_cast<double>(c.BusyNs - p.BusyNs) / (static_cast<double>(elapsed_ns) * c.Capacity);
        }
        if (fraction >= 0.0)
            best = std::max(best, std::min(fraction, 1.0));
    }
    return best;
}

QString ProcessGpuDiagnostics::Summary() const
{
    QStringList parts;
    if (this->DrmPresent)
        parts << QStringLiteral("DRM fdinfo [%1]").arg(this->DrmDrivers.isEmpty() ? QStringLiteral("no client exposes drm-* counters") : this->DrmDrivers.join(QStringLiteral(", ")));
    else
        parts << QStringLiteral("DRM fdinfo [no /dev/dri or /dev/accel]");
    parts << (this->NvmlReady ? QStringLiteral("NVML [%1 device(s)]").arg(this->NvmlDevices) : QStringLiteral("NVML [not available]"));
    return QStringLiteral("Per-process GPU usage sources: ") + parts.join(QStringLiteral("; "))
           + QStringLiteral(" | measured=%1 denied=%2").arg(this->Measured).arg(this->Denied);
}

// ── ProcessGpuSampler ────────────────────────────────────────────────────────

ProcessGpuSampler::ProcessGpuSampler()
    : m_nvml(new Nvml())
{
    this->m_clock.start();
}

ProcessGpuSampler::~ProcessGpuSampler() = default;

void ProcessGpuSampler::Reset()
{
    this->m_pids.clear();
    this->m_pass = 0;
    this->m_drmEnginesSeen = false;
    this->m_lastSources.clear();
}

ProcessGpuDiagnostics ProcessGpuSampler::Diagnostics() const
{
    ProcessGpuDiagnostics d;
    d.DrmPresent = this->m_drmPresent;
    d.NvmlReady = this->m_nvml->IsReady();
    d.NvmlDevices = this->m_nvml->DeviceCount();
    for (auto it = this->m_driverKinds.cbegin(); it != this->m_driverKinds.cend(); ++it)
        d.DrmDrivers << it.key() + QStringLiteral(": ") + it.value();
    d.DrmDrivers.sort();
    d.Measured = this->m_measured;
    d.Denied = this->m_denied;
    return d;
}

bool ProcessGpuSampler::hasDrmSupport()
{
    return QDir(QStringLiteral("/dev/dri")).exists() || QDir(QStringLiteral("/dev/accel")).exists();
}

bool ProcessGpuSampler::listDrmFds(pid_t pid, QVector<int> &fds, bool &denied)
{
    char path[64];
    std::snprintf(path, sizeof(path), "/proc/%d/fd", static_cast<int>(pid));
    DIR *dir = ::opendir(path);
    if (!dir)
    {
        denied = (errno == EACCES || errno == EPERM);
        return false;
    }

    const int dir_fd = ::dirfd(dir);
    while (const dirent *entry = ::readdir(dir))
    {
        if (entry->d_name[0] == '.')
            continue;
        char target[64];
        const ssize_t len = ::readlinkat(dir_fd, entry->d_name, target, sizeof(target) - 1);
        if (len <= 0)
            continue;
        target[len] = '\0';
        if (std::strncmp(target, "/dev/dri/", 9) == 0 || std::strncmp(target, "/dev/accel/", 11) == 0)
            fds.append(std::atoi(entry->d_name));
    }
    ::closedir(dir);
    denied = false;
    return true;
}

bool ProcessGpuSampler::readDrmEngines(pid_t pid, const QVector<int> &fds, QVector<int> &valid_fds, EngineMap &out)
{
    QSet<QString> seen_clients;
    bool any_read = false;

    for (int fd : fds)
    {
        char path[64];
        std::snprintf(path, sizeof(path), "/proc/%d/fdinfo/%d", static_cast<int>(pid), fd);
        const int file = ::open(path, O_RDONLY | O_CLOEXEC);
        if (file < 0)
            continue;
        QByteArray content(MaxFdInfoBytes, Qt::Uninitialized);
        const ssize_t len = ::read(file, content.data(), static_cast<size_t>(content.size()));
        ::close(file);
        if (len <= 0)
            continue;
        content.truncate(static_cast<int>(len));
        valid_fds.append(fd);
        any_read = true;

        DrmClient client;
        if (!ProcessGpuDrm::ParseFdInfo(content, client))
            continue;

        // Remember how each driver reports usage, to explain empty columns.
        if (!client.Driver.isEmpty())
        {
            bool cycles = false;
            for (const DrmEngineCounter &c : std::as_const(client.Engines))
                cycles = cycles || c.HasCycles;
            this->m_driverKinds[client.Driver] = client.Engines.isEmpty() ? QStringLiteral("no engine counters")
                                                 : (cycles ? QStringLiteral("cycles") : QStringLiteral("engine-ns"));
        }

        // The same client is visible through every duplicated fd (and through other fds
        // of the same open file description): count each of them once.
        if (!client.ClientId.isEmpty())
        {
            const QString client_key = client.Pdev + QLatin1Char('/') + client.ClientId;
            if (seen_clients.contains(client_key))
                continue;
            seen_clients.insert(client_key);
        }
        ProcessGpuDrm::MergeClient(out, client);
    }
    return any_read;
}

void ProcessGpuSampler::Sample(QList<Process> &processes)
{
    ++this->m_pass;
    this->m_measured = 0;
    this->m_denied = 0;
    if (this->m_pass == 1)
        this->m_drmPresent = hasDrmSupport();

    QHash<pid_t, double> nvml_usage;
    this->m_nvml->Sample(nvml_usage);
    const bool nvml_ready = this->m_nvml->Ready();
    const bool any_source = nvml_ready || this->m_drmPresent;

    if (!any_source)
    {
        if (this->m_lastSources.isEmpty())
        {
            this->m_lastSources = QStringLiteral("none");
            LOG_INFO(this->Diagnostics().Summary());
        }
        return;
    }

    for (Process &proc : processes)
    {
        if (proc.IsKernelThread)
            continue;

        PidState &state = this->m_pids[proc.PID];
        const bool is_new = (state.LastSeen == 0) || state.StartTimeTicks != proc.StartTimeTicks;
        if (is_new)
        {
            state = PidState();
            state.StartTimeTicks = proc.StartTimeTicks;
        }
        state.LastSeen = this->m_pass;

        double pct = 0.0;
        bool available = this->m_pass > 1;
        bool denied = false;

        if (this->m_drmPresent)
        {
            if (is_new || this->m_pass - state.LastScan >= RescanEveryPasses)
            {
                QVector<int> fds;
                bool denied_now = false;
                if (listDrmFds(proc.PID, fds, denied_now))
                {
                    state.DrmFds = fds;
                    state.Denied = false;
                } else
                {
                    state.DrmFds.clear();
                    state.Denied = denied_now;
                }
                // Stagger rescans so they do not all happen in the same pass.
                state.LastScan = this->m_pass - static_cast<int>(proc.PID % RescanEveryPasses);
            }

            EngineMap current;
            if (!state.DrmFds.isEmpty())
            {
                QVector<int> valid;
                const qint64 now_ns = this->m_clock.nsecsElapsed();
                this->readDrmEngines(proc.PID, state.DrmFds, valid, current);
                if (!current.isEmpty())
                    this->m_drmEnginesSeen = true;
                if (valid.size() != state.DrmFds.size())
                {
                    state.DrmFds = valid;
                    state.LastScan = this->m_pass - RescanEveryPasses; // rescan on the next pass
                }

                const double fraction = ProcessGpuDrm::BusiestEngineFraction(state.Prev, current, now_ns - state.PrevNs);
                const bool measured = fraction >= 0.0;
                if (measured)
                    pct = fraction * 100.0;
                // Not measurable yet: a client that just appeared has no previous counters, and
                // a driver without engine counters never will. Show "unavailable", not a fake 0.
                available = available && measured;

                state.Prev = current;
                state.PrevNs = now_ns;
            } else
            {
                state.Prev.clear();
            }
            // Without any DRM engine counters on this system (e.g. proprietary NVIDIA only) a
            // denied /proc/PID/fd hides nothing: NVML covers every user's processes.
            denied = state.Denied && (this->m_drmEnginesSeen || !nvml_ready);
        }

        const auto nv_it = nvml_usage.constFind(proc.PID);
        if (nv_it != nvml_usage.cend())
        {
            pct = std::max(pct, nv_it.value());
            available = this->m_pass > 1;
            denied = false;
        } else if (nvml_ready && this->m_pass > 1 && !denied)
        {
            available = true;
        }

        proc.GpuPercent = pct;
        proc.GpuAvailable = available && !denied;
        proc.GpuPermissionDenied = denied;
        this->m_measured += proc.GpuAvailable ? 1 : 0;
        this->m_denied += denied ? 1 : 0;
    }

    // Log which sources are in use whenever that changes (the first pass cannot know the drivers yet).
    if (this->m_pass > 1)
    {
        const ProcessGpuDiagnostics d = this->Diagnostics();
        ProcessGpuDiagnostics sources_only = d;   // counters change every pass, sources rarely
        sources_only.Measured = sources_only.Denied = 0;
        const QString sources = sources_only.Summary();
        if (sources != this->m_lastSources)
        {
            this->m_lastSources = sources;
            LOG_INFO(d.Summary());
        }
    }

    // Forget processes that disappeared.
    for (auto it = this->m_pids.begin(); it != this->m_pids.end();)
    {
        if (it.value().LastSeen != this->m_pass)
            it = this->m_pids.erase(it);
        else
            ++it;
    }
}
