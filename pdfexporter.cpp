#include "pdfexporter.h"

#include "qcustomplot.h"

#include <QPageSize>
#include <QPdfWriter>
#include <QSaveFile>

namespace PdfExporter {

namespace {

// 将 firstPage 起的 pageCount 页渲染到 PDF 设备。
// 整个渲染过程复用单个 QCustomPlot；第一页不换页，其余页 newPage。
// 注意：QCustomPlot 是 QWidget，只能在 GUI 线程创建和绘制，
// 因此本函数必须由拥有 QApplication 的 GUI 线程调用。
bool renderPart(QIODevice *device,
                int firstPage,
                int partPageCount,
                int cols,
                int rows,
                const PlotConfigureFn &configurePlot,
                int resolutionDpi,
                int *doneCells,
                int totalCells,
                const ProgressFn &onProgress)
{
    QPdfWriter writer(device);
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

            const int done = ++(*doneCells);
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

    // QCustomPlot 基于 QWidget，QWidget 只能在 GUI 线程创建与绘制。
    // 因此这里不做线程池/分片并行：始终在当前（GUI）线程顺序渲染。
    // 通过 QSaveFile 写入：成功才原子替换目标文件，失败不会留下半个坏 PDF。
    QSaveFile save(fileName);
    if (!save.open(QIODevice::WriteOnly))
    {
        if (errorMessage)
            *errorMessage = QStringLiteral("无法写入文件：%1（%2）").arg(fileName, save.errorString());
        return false;
    }

    const int totalCells = pageCount * cols * rows;
    int done = 0;
    const bool ok = renderPart(&save, 0, pageCount, cols, rows, configurePlot,
                               resolutionDpi, &done, totalCells, onProgress);
    if (!ok)
    {
        save.cancelWriting();
        if (errorMessage)
            *errorMessage = QStringLiteral("PDF 渲染失败：%1").arg(fileName);
        return false;
    }
    if (!save.commit())
    {
        if (errorMessage)
            *errorMessage = QStringLiteral("无法保存文件：%1（%2）").arg(fileName, save.errorString());
        return false;
    }
    return true;
}

} // namespace PdfExporter
