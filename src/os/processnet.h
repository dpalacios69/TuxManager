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

#ifndef OS_PROCESSNET_H
#define OS_PROCESSNET_H

#include "process.h"

#include <QElapsedTimer>
#include <QHash>
#include <QList>
#include <QVector>

namespace OS
{
    /// Cumulative TCP payload of one socket.
    struct TcpSocketCounters
    {
        quint64 Inode { 0 };
        quint64 Bytes { 0 };    ///< Bytes acknowledged (sent) plus bytes received.
    };

    /// Pure functions behind the network source, split out so they can be unit tested.
    namespace ProcessNetDiag
    {
        /// Bytes acked + received from the payload of an INET_DIAG_INFO attribute (the kernel's
        /// struct tcp_info). Returns false when the payload is too short (kernels before 4.1).
        bool ParseTcpInfo(const unsigned char *data, size_t length, quint64 &bytes);

        /// Parse one buffer of sock_diag replies. Sockets without an inode or without byte counters
        /// are skipped. @p done is set on NLMSG_DONE or NLMSG_ERROR, @p error on NLMSG_ERROR.
        void ParseDump(const char *data, size_t length, QVector<TcpSocketCounters> &out, bool &done, bool &error);

        /// Inode of a "socket:[1234]" link target, as read from /proc/PID/fd/N.
        bool ParseSocketLink(const char *target, quint64 &inode);

        /// Bytes moved by a socket between two samples. A socket that was not there before
        /// moved everything it reports; a counter that went backwards (inode reuse) counts as 0.
        quint64 SocketDelta(bool known_before, quint64 previous, quint64 current);
    }

    /// Per-process network throughput without privileges or libraries.
    ///
    /// Reads the byte counters of every TCP socket through netlink sock_diag (the interface
    /// behind `ss -ti`) and attributes each socket to the process holding its inode in
    /// /proc/PID/fd. UDP (QUIC, DNS, games) has no byte counters in the kernel and is NOT counted,
    /// and bytes of sockets that close between two samples are lost. The value is therefore a
    /// lower bound of the real traffic. Loopback traffic is included.
    ///
    /// Not thread safe: use one instance from a single thread.
    class ProcessNetSampler
    {
        public:
            ProcessNetSampler();
            ~ProcessNetSampler();
            ProcessNetSampler(const ProcessNetSampler &) = delete;
            ProcessNetSampler &operator=(const ProcessNetSampler &) = delete;

            /// Fills NetBps / NetAvailable / NetPermissionDenied of every process.
            void Sample(QList<Process> &processes);

            /// Forget all previous samples (call when network metrics get disabled).
            void Reset();

        private:
            struct PidState
            {
                quint64          StartTimeTicks { 0 };
                QVector<quint64> Inodes;
                int              LastScan { 0 };
                int              LastSeen { 0 };
                bool             Denied { false };
            };

            bool querySockets(QHash<quint64, quint64> &sockets) const;
            static bool listSocketInodes(pid_t pid, QVector<quint64> &inodes, bool &denied);

            QHash<pid_t, PidState>      m_pids;
            QHash<quint64, quint64>     m_prevBytes;   ///< inode -> bytes at the previous pass
            QElapsedTimer               m_clock;
            qint64                      m_prevNs { 0 };
            int                         m_pass { 0 };
            bool                        m_reportedFailure { false };
    };
}

#endif // OS_PROCESSNET_H
