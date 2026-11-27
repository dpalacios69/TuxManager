#include "os/processnet.h"

#include <QtTest>

#include <cstring>

#include <linux/inet_diag.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/sock_diag.h>

using namespace OS;
using namespace OS::ProcessNetDiag;

class ProcessNetTest : public QObject
{
    Q_OBJECT

    private:
        /// A tcp_info payload as the kernel sends it: only the two counters matter.
        static QByteArray tcp_info(quint64 acked, quint64 received, int length = 280)
        {
            QByteArray info(length, '\0');
            if (length >= 136)
            {
                std::memcpy(info.data() + 120, &acked, sizeof(acked));
                std::memcpy(info.data() + 128, &received, sizeof(received));
            }
            return info;
        }

        /// One sock_diag reply: nlmsghdr + inet_diag_msg + optional INET_DIAG_INFO attribute.
        static QByteArray reply(quint32 inode, const QByteArray &info, bool with_attribute = true)
        {
            QByteArray body(sizeof(inet_diag_msg), '\0');
            inet_diag_msg *message = reinterpret_cast<inet_diag_msg *>(body.data());
            message->idiag_inode = inode;
            if (with_attribute)
            {
                rtattr attribute;
                attribute.rta_type = INET_DIAG_INFO;
                attribute.rta_len = static_cast<unsigned short>(RTA_LENGTH(info.size()));
                body.append(reinterpret_cast<const char *>(&attribute), sizeof(attribute));
                body.append(info);
                while (body.size() % 4)
                    body.append('\0');
            }
            return netlink(SOCK_DIAG_BY_FAMILY, body);
        }

        static QByteArray netlink(unsigned short type, const QByteArray &body)
        {
            nlmsghdr header;
            std::memset(&header, 0, sizeof(header));
            header.nlmsg_len = static_cast<__u32>(NLMSG_LENGTH(body.size()));
            header.nlmsg_type = type;
            QByteArray message(reinterpret_cast<const char *>(&header), sizeof(header));
            message.append(body);
            while (message.size() % 4)
                message.append('\0');
            return message;
        }

    private slots:
        // ── tcp_info ─────────────────────────────────────────────────────────

        void tcpInfoSumsAckedAndReceived()
        {
            quint64 bytes = 0;
            const QByteArray info = tcp_info(1000, 250);
            QVERIFY(ParseTcpInfo(reinterpret_cast<const unsigned char *>(info.constData()), static_cast<size_t>(info.size()), bytes));
            QCOMPARE(bytes, quint64(1250));
        }

        void tcpInfoOfOldKernelIsRejected()
        {
            // Before Linux 4.1 tcp_info ends before the byte counters.
            quint64 bytes = 0;
            const QByteArray info = tcp_info(1000, 250, 104);
            QVERIFY(!ParseTcpInfo(reinterpret_cast<const unsigned char *>(info.constData()), static_cast<size_t>(info.size()), bytes));
            QVERIFY(!ParseTcpInfo(nullptr, 0, bytes));
        }

        void tcpInfoHandlesLargeCounters()
        {
            quint64 bytes = 0;
            const QByteArray info = tcp_info(Q_UINT64_C(5000000000), Q_UINT64_C(7000000000));
            QVERIFY(ParseTcpInfo(reinterpret_cast<const unsigned char *>(info.constData()), static_cast<size_t>(info.size()), bytes));
            QCOMPARE(bytes, Q_UINT64_C(12000000000));
        }

        // ── netlink dump ─────────────────────────────────────────────────────

        void dumpReturnsSocketsUntilDone()
        {
            QByteArray buffer = reply(1111, tcp_info(10, 20)) + reply(2222, tcp_info(300, 400)) + netlink(NLMSG_DONE, QByteArray(4, '\0'));
            QVector<TcpSocketCounters> sockets;
            bool done = false, error = false;
            ParseDump(buffer.constData(), static_cast<size_t>(buffer.size()), sockets, done, error);
            QVERIFY(done);
            QVERIFY(!error);
            QCOMPARE(sockets.size(), 2);
            QCOMPARE(sockets[0].Inode, quint64(1111));
            QCOMPARE(sockets[0].Bytes, quint64(30));
            QCOMPARE(sockets[1].Inode, quint64(2222));
            QCOMPARE(sockets[1].Bytes, quint64(700));
        }

        void dumpWithoutDoneKeepsWaiting()
        {
            const QByteArray buffer = reply(1111, tcp_info(1, 1));
            QVector<TcpSocketCounters> sockets;
            bool done = false, error = false;
            ParseDump(buffer.constData(), static_cast<size_t>(buffer.size()), sockets, done, error);
            QVERIFY(!done);
            QCOMPARE(sockets.size(), 1);
        }

        void dumpSkipsSocketsWithoutOwnerOrCounters()
        {
            const QByteArray buffer = reply(0, tcp_info(5, 5))               // TIME_WAIT: no inode
                                      + reply(3333, QByteArray(), false)     // no INET_DIAG_INFO attribute
                                      + reply(4444, tcp_info(5, 5, 100))     // old kernel: short tcp_info
                                      + reply(5555, tcp_info(7, 3));
            QVector<TcpSocketCounters> sockets;
            bool done = false, error = false;
            ParseDump(buffer.constData(), static_cast<size_t>(buffer.size()), sockets, done, error);
            QCOMPARE(sockets.size(), 1);
            QCOMPARE(sockets[0].Inode, quint64(5555));
        }

        void dumpReportsKernelError()
        {
            const QByteArray buffer = netlink(NLMSG_ERROR, QByteArray(sizeof(nlmsgerr), '\0'));
            QVector<TcpSocketCounters> sockets;
            bool done = false, error = false;
            ParseDump(buffer.constData(), static_cast<size_t>(buffer.size()), sockets, done, error);
            QVERIFY(done);
            QVERIFY(error);
            QVERIFY(sockets.isEmpty());
        }

        void dumpSurvivesTruncatedAndEmptyBuffers()
        {
            const QByteArray full = reply(1111, tcp_info(10, 20));
            for (int length = 0; length < full.size(); ++length)
            {
                QVector<TcpSocketCounters> sockets;
                bool done = false, error = false;
                ParseDump(full.constData(), static_cast<size_t>(length), sockets, done, error);
                QVERIFY(sockets.size() <= 1);   // must neither crash nor invent sockets
            }
            QVector<TcpSocketCounters> sockets;
            bool done = false, error = false;
            ParseDump(nullptr, 0, sockets, done, error);
            QVERIFY(sockets.isEmpty());
        }

        // ── /proc/PID/fd link targets ────────────────────────────────────────

        void socketLinkIsParsed()
        {
            quint64 inode = 0;
            QVERIFY(ParseSocketLink("socket:[123456]", inode));
            QCOMPARE(inode, quint64(123456));
        }

        void otherLinkTargetsAreRejected()
        {
            quint64 inode = 0;
            QVERIFY(!ParseSocketLink("/dev/dri/renderD128", inode));
            QVERIFY(!ParseSocketLink("anon_inode:[eventfd]", inode));
            QVERIFY(!ParseSocketLink("pipe:[1234]", inode));
            QVERIFY(!ParseSocketLink("socket:[]", inode));
            QVERIFY(!ParseSocketLink("socket:[12", inode));
            QVERIFY(!ParseSocketLink("socket:[x1]", inode));
            QVERIFY(!ParseSocketLink(nullptr, inode));
        }

        // ── Delta ────────────────────────────────────────────────────────────

        void deltaOfKnownSocketIsTheDifference()
        {
            QCOMPARE(SocketDelta(true, 1000, 1600), quint64(600));
            QCOMPARE(SocketDelta(true, 1000, 1000), quint64(0));
        }

        void deltaOfNewSocketIsEverythingItMoved()
        {
            QCOMPARE(SocketDelta(false, 0, 4200), quint64(4200));
        }

        void deltaOfCounterGoingBackwardsIsZero()
        {
            // The inode was reused by a new connection between two samples.
            QCOMPARE(SocketDelta(true, 9000, 100), quint64(0));
        }
};

QTEST_APPLESS_MAIN(ProcessNetTest)
#include "processnet_test.moc"
