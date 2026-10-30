#include "pdfexporter.h"

#include "pdfmerge.h"
#include "qcustomplot.h"

#include <QAtomicInt>
#include <QDir>
#include <QPageSize>
#include <QPdfWriter>
#include <QThread>
#include <QtConcurrent/QtConcurrent>

namespace PdfExporter {

namespace {

// 渲染一个分片（firstPage 起 pageCount 页）到独立 PDF 文件。
// 该分片内复用单个 QCustomPlot；首片第一页不换页，其余页 newPage。
// doneCells / totalCells 用于跨分片合并进度（原子计数）。
bool renderPart(const QString &partFile,
                int firstPage,
                int partPageCount,
                int cols,
                int rows,
                const PlotConfigureFn &configurePlot,
                int resolutionDpi,
                std::atomic<int> *doneCells,
                int totalCells,
                const ProgressFn &onProgress)
{
    QPdfWriter writer(partFile);
    writer.setResolution(resolutionDpi);
    writer.setPageSize(QPageSize(QSizeF(210.0, 297.0), QPageSize::Millimeter));
    writer.setTitle(QStringLiteral("QCustomPlot 多页导出"));
    writer.setCreator(QStringLiteral("CustomPlotPdf / Qt %1").arg(QT_VERSION_STR));

    QCPPainter painter(&writer);
    if (!painter.isActive())
        return false;
    painter.setMode(QCPPainter::pmVectorized);

    const double mm2px    = resolutionDpi / 25.4;
    const double pageWmm  = 210.0;
    const double pageHmm  = 297.0;
    const double marginX  = 12.0;
    const double marginY  = 12.0;
    const double gap      =  6.0;
    const double cellWmm  = (pageWmm - 2.0 * marginX - (cols - 1) * gap) / cols;
    const double cellHmm  = (pageHmm - 2.0 * marginY - (rows - 1) * gap) / rows;
    const int cellWpx     = qRound(cellWmm * mm2px);
    const int cellHpx     = qRound(cellHmm * mm2px);

    QCustomPlot plot;
    plot.setFixedSize(cellWpx, cellHpx);

    for (int p = 0; p < partPageCount; ++p)
    {
        const int page = firstPage + p;
        if (p > 0)
            writer.newPage();

        for (int cell = 0; cell < cols * rows; ++cell)
        {
            configurePlot(&plot, page, cell);

            const int col = cell % cols;
            const int row = cell / cols;
            const double originXpx = (marginX + col * (cellWmm + gap)) * mm2px;
            const double originYpx = (marginY + row * (cellHmm + gap)) * mm2px;

            painter.save();
            painter.translate(originXpx, originYpx);
            plot.toPainter(&painter, cellWpx, cellHpx);
            painter.restore();

            const int done = doneCells->fetch_add(1) + 1;
            if (onProgress)
                onProgress(done, totalCells);
        }
    }

    const bool ok = painter.isActive();
    painter.end();
    return ok;
}

} // namespace

bool exportPages(const QString &fileName,
                 int pageCount,
                 int cols,
                 int rows,
                 const PlotConfigureFn &configurePlot,
                 const ProgressFn &onProgress,
                 QString *errorMessage,
                 int resolutionDpi)
{
    if (pageCount <= 0 || cols <= 0 || rows <= 0 || !configurePlot)
    {
        if (errorMessage)
            *errorMessage = QStringLiteral("参数错误：页数/格数必须为正，且必须提供 configurePlot 回调");
        return false;
    }

    // —— 分片数量：按 CPU 核数并行（上限 8 片），页数少时自动退化为 1 片 ——
    const int parts = qBound(1, qMin(QThread::idealThreadCount(), 8), pageCount);
    const int totalCells = pageCount * cols * rows;

    if (parts == 1)
    {
        std::atomic<int> done(0);
        return renderPart(fileName, 0, pageCount, cols, rows, configurePlot,
                          resolutionDpi, &done, totalCells, onProgress);
    }

    // —— 建立临时目录与分片文件列表 ——
    const QString tmpDir = QDir::tempPath() + QStringLiteral("/CustomPlotPdf_parts_%1")
                           .arg(QCoreApplication::applicationPid());
    QDir().mkpath(tmpDir);
    QStringList partFiles;
    for (int i = 0; i < parts; ++i)
        partFiles.append(tmpDir + QStringLiteral("/part_%1.pdf").arg(i));

    // —— 页数均分 ——
    QVector<int> firstPages(parts), partCounts(parts);
    int remain = pageCount;
    for (int i = 0; i < parts; ++i)
    {
        partCounts[i] = remain / (parts - i);
        remain -= partCounts[i];
    }
    int first = 0;
    for (int i = 0; i < parts; ++i)
    {
        firstPages[i] = first;
        first += partCounts[i];
    }

    // —— 并行渲染各分片（独立 QPdfWriter + 复用 plot，线程安全） ——
    std::atomic<int> doneCells(0);
    QVector<bool> partOk(parts, false);
    {
        QVector<int> idx(parts);
        for (int i = 0; i < parts; ++i) idx[i] = i;

        QtConcurrent::map(idx, [&](int i) {
            partOk[i] = renderPart(partFiles.at(i), firstPages.at(i), partCounts.at(i),
                                   cols, rows, configurePlot, resolutionDpi,
                                   &doneCells, totalCells, onProgress);
        }).waitForFinished();
    }

    bool allOk = true;
    int failedPart = 0;
    for (int i = 0; i < parts; ++i)
        if (!partOk.at(i)) { allOk = false; failedPart = i; break; }
    if (!allOk)
    {
        if (errorMessage)
            *errorMessage = QStringLiteral("分片渲染失败（分片 %1/%2）").arg(failedPart + 1).arg(parts);
        QDir(tmpDir).removeRecursively();
        return false;
    }

    // —— 合并分片为单个 PDF（引用重编号 + 内容流压缩 + 重建 Pages 树） ——
    if (onProgress)
        onProgress(totalCells, totalCells); // 渲染完成，进入合并阶段
    QString mergeErr;
    const bool merged = PdfMerger::mergeAndCompress(partFiles, fileName, &mergeErr, nullptr, 9);
    if (qEnvironmentVariableIsEmpty("CUSTOM_PLOT_PDF_KEEP_PARTS"))
        QDir(tmpDir).removeRecursively();

    if (!merged)
    {
        if (errorMessage)
            *errorMessage = QStringLiteral("PDF 合并失败：%1").arg(mergeErr);
        return false;
    }
    return true;
}

} // namespace PdfExporter
