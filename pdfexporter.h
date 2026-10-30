#ifndef PDFEXPORTER_H
#define PDFEXPORTER_H

#include <QString>
#include <functional>

class QCustomPlot;

namespace PdfExporter
{
    // configurePlot(plot, page, cell)：在复用 plot 上更新为第 page 页第 cell 格的数据。
    // 导出器内部只创建一个 QCustomPlot 实例反复复用（首次调用完成初始化），
    // 避免每格 new/delete 的开销，显著提升多页导出速度。
    using PlotConfigureFn = std::function<void(QCustomPlot *, int page, int cell)>;
    // onProgress(done, total)：每完成一个格子回调一次，done 为已完成的格子数。
    // 回调发生在导出线程中，调用方若需更新 UI 应自行切回主线程。
    using ProgressFn = std::function<void(int done, int total)>;

    // 将多页 PDF 导出到 fileName：
    //   - 每页 cols x rows 个图（默认 2x2 田字格），A4 纵向、带页边距
    //   - 渲染方式：QCPPainter 包裹 QPdfWriter，逐格 painter.translate 后调用
    //     QCustomPlot::toPainter(painter, cellWpx, cellHpx)，纯矢量输出
    //   - 页数 pageCount 即 N，默认由调用方指定（GUI 微调框 / 命令行 --pages），
    //     后续需要更多页只需改调用处，无需改动本函数。
    // 返回 true 表示成功；失败时通过 errorMessage 返回原因（可为空指针）。
    // resolutionDpi 必须保持 72（逻辑坐标与设备 1:1，Qt 5.15 PDF 引擎在该分辨率下
    // 才能正确输出 QCustomPlot 的 CJK 文本；矢量输出不依赖高分辨率）。
    bool exportPages(const QString &fileName,
                     int pageCount,
                     int cols,
                     int rows,
                     const PlotConfigureFn &configurePlot,
                     const ProgressFn &onProgress = nullptr,
                     QString *errorMessage = nullptr,
                     int resolutionDpi = 72);
}

#endif // PDFEXPORTER_H
