#include "os/processgpu.h"

#include <QFile>
#include <QtTest>

using namespace OS;
using namespace OS::ProcessGpuDrm;

class ProcessGpuTest : public QObject
{
    Q_OBJECT

    private:
        static QByteArray fixture(const char *name)
        {
            QFile f(QStringLiteral(FIXTURES_DIR "/") + QString::fromLatin1(name));
            if (!f.open(QIODevice::ReadOnly))
                qFatal("missing fixture %s", name);
            return f.readAll();
        }

        static DrmEngineMap engines(qint64 busy_ns, unsigned capacity = 1)
        {
            DrmEngineMap m;
            m[QStringLiteral("0000:03:00.0/gfx")].BusyNs = static_cast<quint64>(busy_ns);
            m[QStringLiteral("0000:03:00.0/gfx")].Capacity = capacity;
            return m;
        }

    private slots:
        // ── Parsing: one test per driver layout ──────────────────────────────

        void parseAmdgpu()
        {
            DrmClient c;
            QVERIFY(ParseFdInfo(fixture("amdgpu.fdinfo"), c));
            QCOMPARE(c.Driver, QStringLiteral("amdgpu"));
            QCOMPARE(c.Pdev, QStringLiteral("0000:03:00.0"));
            QCOMPARE(c.ClientId, QStringLiteral("5"));
            QCOMPARE(c.Engines.size(), 6);
            QCOMPARE(c.Engines.value("gfx").BusyNs, quint64(2000000000));
            QCOMPARE(c.Engines.value("dec").BusyNs, quint64(100000000));
            QVERIFY(!c.Engines.value("gfx").HasCycles);
        }

        void parseI915WithCapacity()
        {
            DrmClient c;
            QVERIFY(ParseFdInfo(fixture("i915.fdinfo"), c));
            QCOMPARE(c.Driver, QStringLiteral("i915"));
            QCOMPARE(c.Engines.size(), 4);   // capacity line is not an engine of its own
            QCOMPARE(c.Engines.value("video").BusyNs, quint64(1000000000));
            QCOMPARE(c.Engines.value("video").Capacity, 2u);
            QCOMPARE(c.Engines.value("render").Capacity, 1u);
            QVERIFY(c.Engines.contains("video-enhance"));
        }

        void parseXeCycles()
        {
            DrmClient c;
            QVERIFY(ParseFdInfo(fixture("xe.fdinfo"), c));
            QCOMPARE(c.Driver, QStringLiteral("xe"));
            QCOMPARE(c.Engines.size(), 5);
            QVERIFY(c.Engines.value("rcs").HasCycles);
            QCOMPARE(c.Engines.value("rcs").Cycles, quint64(250000));
            QCOMPARE(c.Engines.value("rcs").TotalCycles, quint64(1000000));
        }

        void parseMsmUsesNanosecondsNotCycles()
        {
            // msm reports drm-cycles-gpu without drm-total-cycles-gpu: cycles must not be used.
            DrmClient c;
            QVERIFY(ParseFdInfo(fixture("msm.fdinfo"), c));
            QCOMPARE(c.Engines.size(), 1);
            QVERIFY(!c.Engines.value("gpu").HasCycles);
            QCOMPARE(c.Engines.value("gpu").BusyNs, quint64(3000000000));
        }

        void parseNvidiaProprietaryHasNoDrmKeys()
        {
            // Real capture: the proprietary driver exposes no drm-* keys, so NVML is the only source.
            DrmClient c;
            QVERIFY(!ParseFdInfo(fixture("nvidia-proprietary.fdinfo"), c));
            QVERIFY(c.Engines.isEmpty());
        }

        void parseRegularFileIsNotDrm()
        {
            DrmClient c;
            QVERIFY(!ParseFdInfo(fixture("regular-file.fdinfo"), c));
        }

        void parseToleratesGarbage()
        {
            DrmClient c;
            QVERIFY(ParseFdInfo("drm-driver: x\ndrm-engine-gfx: abc ns\ndrm-engine-: 5 ns\n:\n\n\xff\xfe\ndrm-client-id\n", c));
            QCOMPARE(c.Engines.value("gfx").BusyNs, quint64(0));
            QVERIFY(!ParseFdInfo(QByteArray(), c));
        }

        void capacityWithoutCounterIsNotAnEngine()
        {
            DrmClient c;
            QVERIFY(ParseFdInfo("drm-driver: i915\ndrm-engine-capacity-video: 2\n", c));
            QVERIFY(c.Engines.isEmpty());
        }

        // ── Utilization math ─────────────────────────────────────────────────

        void halfBusyIsFiftyPercent()
        {
            const double f = BusiestEngineFraction(engines(0), engines(500000000), 1000000000);
            QVERIFY(qAbs(f - 0.5) < 1e-9);
        }

        void capacityDividesBusyTime()
        {
            // Two identical engines, 1 s of busy time in 1 s: one of the two instances is saturated.
            const double f = BusiestEngineFraction(engines(0, 2), engines(1000000000, 2), 1000000000);
            QVERIFY(qAbs(f - 0.5) < 1e-9);
        }

        void overUnityIsClamped()
        {
            QCOMPARE(BusiestEngineFraction(engines(0), engines(5000000000), 1000000000), 1.0);
        }

        void idleIsZeroNotUnavailable()
        {
            QCOMPARE(BusiestEngineFraction(engines(700), engines(700), 1000000000), 0.0);
        }

        void counterGoingBackwardsIsNotMeasurable()
        {
            // A client closed between samples: the summed counter drops.
            QVERIFY(BusiestEngineFraction(engines(900), engines(100), 1000000000) < 0.0);
        }

        void noElapsedTimeIsNotMeasurable()
        {
            QVERIFY(BusiestEngineFraction(engines(0), engines(100), 0) < 0.0);
        }

        void newEngineWithoutPreviousSampleIsNotMeasurable()
        {
            QVERIFY(BusiestEngineFraction(DrmEngineMap(), engines(100), 1000000000) < 0.0);
        }

        void busiestEngineWins()
        {
            DrmEngineMap prev, cur;
            prev["a/gfx"].BusyNs = 0;     cur["a/gfx"].BusyNs = 100000000;   // 10 %
            prev["a/enc"].BusyNs = 0;     cur["a/enc"].BusyNs = 800000000;   // 80 %
            const double f = BusiestEngineFraction(prev, cur, 1000000000);
            QVERIFY(qAbs(f - 0.8) < 1e-9);
        }

        void xeCyclesAreARatioOfTotalCycles()
        {
            DrmClient a, b;
            QVERIFY(ParseFdInfo(fixture("xe.fdinfo"), a));
            b = a;
            b.Engines["rcs"].Cycles = 250000 + 300000;
            b.Engines["rcs"].TotalCycles = 1000000 + 1000000;   // 300k busy of 1M elapsed cycles
            DrmEngineMap prev, cur;
            MergeClient(prev, a);
            MergeClient(cur, b);
            const double f = BusiestEngineFraction(prev, cur, 1000000000);
            QVERIFY(qAbs(f - 0.3) < 1e-9);
        }

        void cyclesIgnoreWallClock()
        {
            DrmEngineMap prev, cur;
            prev["a/rcs"] = { 0, 0, 100, 1, true };
            cur["a/rcs"]  = { 0, 50, 200, 1, true };
            // Elapsed time is irrelevant (even zero) for cycle based drivers.
            QVERIFY(qAbs(BusiestEngineFraction(prev, cur, 0) - 0.5) < 1e-9);
        }

        void mergeSumsClientsOfTheSameProcess()
        {
            DrmClient a, b;
            a.Pdev = b.Pdev = QStringLiteral("0000:03:00.0");
            a.Engines["gfx"].BusyNs = 100;
            b.Engines["gfx"].BusyNs = 250;
            DrmEngineMap totals;
            MergeClient(totals, a);
            MergeClient(totals, b);
            QCOMPARE(totals.size(), 1);
            QCOMPARE(totals.value("0000:03:00.0/gfx").BusyNs, quint64(350));
        }

        void sameEngineNameOnDifferentGpusStaysSeparate()
        {
            DrmClient a, b;
            a.Pdev = QStringLiteral("0000:03:00.0");
            b.Pdev = QStringLiteral("0000:0a:00.0");
            a.Engines["gfx"].BusyNs = 1;
            b.Engines["gfx"].BusyNs = 1;
            DrmEngineMap totals;
            MergeClient(totals, a);
            MergeClient(totals, b);
            QCOMPARE(totals.size(), 2);
        }

        // ── Diagnostics ──────────────────────────────────────────────────────

        void summaryExplainsMissingSources()
        {
            ProcessGpuDiagnostics d;
            const QString s = d.Summary();
            QVERIFY(s.contains("no /dev/dri"));
            QVERIFY(s.contains("NVML [not available]"));
        }

        void summaryListsDriversAndCounts()
        {
            ProcessGpuDiagnostics d;
            d.DrmPresent = true;
            d.NvmlReady = true;
            d.NvmlDevices = 1;
            d.DrmDrivers << "amdgpu: engine-ns" << "xe: cycles";
            d.Measured = 7;
            const QString s = d.Summary();
            QVERIFY(s.contains("amdgpu: engine-ns, xe: cycles"));
            QVERIFY(s.contains("NVML [1 device(s)]"));
            QVERIFY(s.contains("measured=7"));
        }
};

QTEST_APPLESS_MAIN(ProcessGpuTest)
#include "processgpu_test.moc"
