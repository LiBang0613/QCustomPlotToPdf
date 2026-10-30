#include <QApplication>
#include <QCommandLineParser>
#include <QDateTime>
#include <QFile>
#include <QTextStream>
#include <QTimer>

#include "mainwindow.h"
#include "pdfexporter.h"
#include "plotfactory.h"

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("CustomPlotPdf"));
    QApplication::setApplicationVersion(QStringLiteral("1.0"));

    // 命令行参数：
    //   --export <pdf路径> [--pages N]   命令行直接导出后退出（不创建窗口）
    //   --selftest <pdf路径> [--pages N] GUI 异步导出自测：不弹窗、自动退出，
    //      导出期间记录主线程心跳计数，用于验证异步导出不阻塞界面
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("QCustomPlot 多页 PDF 导出（每页 4 图，田字格）"));
    parser.addHelpOption();
    QCommandLineOption exportOpt(QStringLiteral("export"), QStringLiteral("导出 PDF 后退出，参数为输出文件路径"), QStringLiteral("path"));
    QCommandLineOption selftestOpt(QStringLiteral("selftest"), QStringLiteral("GUI 异步导出自测（不弹窗自动退出）"), QStringLiteral("path"));
    QCommandLineOption pagesOpt(QStringLiteral("pages"), QStringLiteral("导出页数 N（默认 2）"), QStringLiteral("n"));
    parser.addOption(exportOpt);
    parser.addOption(selftestOpt);
    parser.addOption(pagesOpt);
    parser.process(app);

    int pages = 2; // 暂时 N=2，后续可通过 --pages 设置
    bool okPages = true;
    if (parser.isSet(pagesOpt))
    {
        pages = parser.value(pagesOpt).toInt(&okPages);
        if (!okPages || pages <= 0)
        {
            qWarning().noquote() << "Invalid --pages value, must be a positive integer.";
            return 1;
        }
    }

    // —— 命令行直接导出（同步，适合批处理/CI）——
    if (parser.isSet(exportOpt))
    {
        QString err;
        const bool ok = PdfExporter::exportPages(
            parser.value(exportOpt), pages, 2, 2,
            [](QCustomPlot *p, int page, int cell) { updateSineCosPlot(p, page, cell); },
            nullptr, &err);
        if (!ok)
        {
            qWarning().noquote() << "Export failed:" << err;
            return 1;
        }
        qInfo().noquote() << "Export OK:" << parser.value(exportOpt) << "pages:" << pages;
        return 0;
    }

    // —— GUI 异步导出自测 ——
    if (parser.isSet(selftestOpt))
    {
        MainWindow w;
        w.setShowDialogs(false);

        // 主线程心跳：若导出期间界面被卡死，心跳计数会停止增长
        int ticks = 0;
        QTimer heartbeat;
        QObject::connect(&heartbeat, &QTimer::timeout, [&ticks]() { ++ticks; });
        heartbeat.start(50);

        QObject::connect(&w, &MainWindow::exportFinished,
                         [&](bool ok, qint64 elapsedMs) {
            heartbeat.stop();
            // GUI 子系统程序无控制台，结果写入 exe 同目录日志
            QFile logFile(QApplication::applicationDirPath() + QStringLiteral("/CustomPlotPdf.log"));
            if (logFile.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
            {
                QTextStream ts(&logFile);
                ts << QDateTime::currentDateTime().toString(Qt::ISODate)
                   << QStringLiteral(" SELFTEST ok=") << ok
                   << QStringLiteral(" elapsed_ms=") << elapsedMs
                   << QStringLiteral(" ui_ticks=") << ticks
                   << QStringLiteral(" progress_final=") << w.exportProgressValue()
                   << QStringLiteral(" error=") << (ok ? QStringLiteral("-") : w.lastExportError())
                   << QStringLiteral(" file=") << parser.value(selftestOpt)
                   << QStringLiteral(" pages=") << pages
                   << QStringLiteral("\n");
                logFile.close();
            }
            QApplication::exit(ok ? 0 : 1);
        });

        QTimer::singleShot(0, [&]() { w.exportToFile(parser.value(selftestOpt), pages); });
        return app.exec();
    }

    MainWindow w;
    w.show();
    return app.exec();
}
