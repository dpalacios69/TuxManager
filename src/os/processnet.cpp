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

#include "processnet.h"

#include "../logger.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <dirent.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <linux/inet_diag.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/sock_diag.h>

using namespace OS;

namespace
{
    /// Sockets are re-scanned every this many passes (new connections are attributed with that delay).
    constexpr int RescanEveryPasses = 5;
    constexpr unsigned TcpListenState = 10;

    // Offsets inside the kernel's struct tcp_info (an append-only UAPI structure). Spelled out here
    // instead of including <linux/tcp.h> so that old kernel headers still build.
    constexpr size_t TcpInfoBytesAckedOffset = 120;
    constexpr size_t TcpInfoBytesReceivedOffset = 128;
    constexpr size_t TcpInfoMinimumLength = 136;
}

bool ProcessNetDiag::ParseTcpInfo(const unsigned char *data, size_t length, quint64 &bytes)
{
    if (!data || length < TcpInfoMinimumLength)
        return false;
    quint64 acked = 0;
    quint64 received = 0;
    std::memcpy(&acked, data + TcpInfoBytesAckedOffset, sizeof(acked));
    std::memcpy(&received, data + TcpInfoBytesReceivedOffset, sizeof(received));
    bytes = acked + received;
    return true;
}

void ProcessNetDiag::ParseDump(const char *data, size_t length, QVector<TcpSocketCounters> &out, bool &done, bool &error)
{
    const nlmsghdr *header = reinterpret_cast<const nlmsghdr *>(data);
    int remaining = static_cast<int>(length);

    for (; NLMSG_OK(header, remaining); header = NLMSG_NEXT(header, remaining))
    {
        if (header->nlmsg_type == NLMSG_DONE)
        {
            done = true;
            return;
        }
        if (header->nlmsg_type == NLMSG_ERROR)
        {
            done = true;
            error = true;
            return;
        }
        if (header->nlmsg_type != SOCK_DIAG_BY_FAMILY || header->nlmsg_len < NLMSG_LENGTH(sizeof(inet_diag_msg)))
            continue;

        const inet_diag_msg *message = reinterpret_cast<const inet_diag_msg *>(NLMSG_DATA(header));
        if (message->idiag_inode == 0)   // TIME_WAIT and other sockets without an owner
            continue;

        int attribute_length = static_cast<int>(header->nlmsg_len - NLMSG_LENGTH(sizeof(inet_diag_msg)));
        for (const rtattr *attribute = reinterpret_cast<const rtattr *>(message + 1);
             RTA_OK(attribute, attribute_length);
             attribute = RTA_NEXT(attribute, attribute_length))
        {
            if (attribute->rta_type != INET_DIAG_INFO)
                continue;
            quint64 bytes = 0;
            if (ParseTcpInfo(static_cast<const unsigned char *>(RTA_DATA(attribute)), RTA_PAYLOAD(attribute), bytes))
                out.append({ message->idiag_inode, bytes });
            break;
        }
    }
}

bool ProcessNetDiag::ParseSocketLink(const char *target, quint64 &inode)
{
    static const char prefix[] = "socket:[";
    if (!target || std::strncmp(target, prefix, sizeof(prefix) - 1) != 0)
        return false;
    char *end = nullptr;
    const unsigned long long value = std::strtoull(target + sizeof(prefix) - 1, &end, 10);
    if (end == target + sizeof(prefix) - 1 || *end != ']')
        return false;
    inode = value;
    return true;
}

quint64 ProcessNetDiag::SocketDelta(bool known_before, quint64 previous, quint64 current)
{
    if (!known_before)
        return current;
    return current >= previous ? current - previous : 0;
}

// ── ProcessNetSampler ────────────────────────────────────────────────────────

ProcessNetSampler::ProcessNetSampler()
{
    this->m_clock.start();
}

ProcessNetSampler::~ProcessNetSampler() = default;

void ProcessNetSampler::Reset()
{
    this->m_pids.clear();
    this->m_prevBytes.clear();
    this->m_pass = 0;
}

bool ProcessNetSampler::querySockets(QHash<quint64, quint64> &sockets) const
{
    const int fd = ::socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_SOCK_DIAG);
    if (fd < 0)
        return false;

    timeval timeout;
    timeout.tv_sec = 0;
    timeout.tv_usec = 300000;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    bool any_family_ok = false;
    QByteArray buffer(256 * 1024, Qt::Uninitialized);

    for (const int family : { AF_INET, AF_INET6 })
    {
        struct
        {
            nlmsghdr header;
            inet_diag_req_v2 request;
        } message;
        std::memset(&message, 0, sizeof(message));
        message.header.nlmsg_len = sizeof(message);
        message.header.nlmsg_type = SOCK_DIAG_BY_FAMILY;
        message.header.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
        message.request.sdiag_family = static_cast<__u8>(family);
        message.request.sdiag_protocol = IPPROTO_TCP;
        message.request.idiag_ext = 1u << (INET_DIAG_INFO - 1);
        message.request.idiag_states = 0xffffffffu & ~(1u << TcpListenState);   // listening sockets move no data

        sockaddr_nl kernel;
        std::memset(&kernel, 0, sizeof(kernel));
        kernel.nl_family = AF_NETLINK;
        if (::sendto(fd, &message, sizeof(message), 0, reinterpret_cast<sockaddr *>(&kernel), sizeof(kernel)) < 0)
            continue;

        bool done = false;
        bool error = false;
        while (!done)
        {
            const ssize_t length = ::recv(fd, buffer.data(), static_cast<size_t>(buffer.size()), 0);
            if (length <= 0)
                break;
            QVector<TcpSocketCounters> parsed;
            ProcessNetDiag::ParseDump(buffer.constData(), static_cast<size_t>(length), parsed, done, error);
            for (const TcpSocketCounters &socket : std::as_const(parsed))
                sockets.insert(socket.Inode, socket.Bytes);
        }
        any_family_ok = any_family_ok || (done && !error);
    }

    ::close(fd);
    return any_family_ok;
}

bool ProcessNetSampler::listSocketInodes(pid_t pid, QVector<quint64> &inodes, bool &denied)
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
        const ssize_t length = ::readlinkat(dir_fd, entry->d_name, target, sizeof(target) - 1);
        if (length <= 0)
            continue;
        target[length] = '\0';
        quint64 inode = 0;
        if (ProcessNetDiag::ParseSocketLink(target, inode))
            inodes.append(inode);
    }
    ::closedir(dir);
    denied = false;
    return true;
}

void ProcessNetSampler::Sample(QList<Process> &processes)
{
    ++this->m_pass;

    QHash<quint64, quint64> sockets;
    if (!this->querySockets(sockets))
    {
        if (!this->m_reportedFailure)
        {
            this->m_reportedFailure = true;
            LOG_WARN(QStringLiteral("Per-process network usage unavailable: netlink sock_diag could not be read"));
        }
        return;
    }
    const qint64 now_ns = this->m_clock.nsecsElapsed();
    const qint64 elapsed_ns = now_ns - this->m_prevNs;

    // Who holds which socket. File descriptors are re-read only every few passes.
    QHash<quint64, pid_t> owner;
    for (const Process &proc : std::as_const(processes))
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

        if (is_new || this->m_pass - state.LastScan >= RescanEveryPasses)
        {
            QVector<quint64> inodes;
            bool denied_now = false;
            if (listSocketInodes(proc.PID, inodes, denied_now))
            {
                state.Inodes = inodes;
                state.Denied = false;
            } else
            {
                state.Inodes.clear();
                state.Denied = denied_now;
            }
            // Stagger rescans so they do not all happen in the same pass.
            state.LastScan = this->m_pass - static_cast<int>(proc.PID % RescanEveryPasses);
        }
        for (const quint64 inode : std::as_const(state.Inodes))
            owner.insert(inode, proc.PID);
    }

    // Bytes moved by each process since the previous pass.
    QHash<pid_t, quint64> moved;
    if (this->m_pass > 1)
    {
        for (auto it = sockets.cbegin(); it != sockets.cend(); ++it)
        {
            const auto before = this->m_prevBytes.constFind(it.key());
            const quint64 delta = ProcessNetDiag::SocketDelta(before != this->m_prevBytes.cend(),
                                                              before != this->m_prevBytes.cend() ? before.value() : 0,
                                                              it.value());
            const pid_t pid = owner.value(it.key(), 0);
            if (delta > 0 && pid > 0)
                moved[pid] += delta;
        }
    }
    this->m_prevBytes = sockets;
    this->m_prevNs = now_ns;

    const bool measurable = this->m_pass > 1 && elapsed_ns > 0;
    for (Process &proc : processes)
    {
        if (proc.IsKernelThread)
            continue;
        const PidState &state = this->m_pids.value(proc.PID);
        proc.NetPermissionDenied = state.Denied;
        proc.NetAvailable = measurable && !state.Denied;
        proc.NetBps = proc.NetAvailable
                      ? static_cast<double>(moved.value(proc.PID, 0)) * 1e9 / static_cast<double>(elapsed_ns)
                      : 0.0;
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
