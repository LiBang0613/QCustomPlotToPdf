#include "mainwindow.h"

#include "pdfexporter.h"
#include "plotfactory.h"
#include "qcustomplot.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

namespace {
// 每页田字格：2 列 x 2 行（网格布局与导出器必须保持一致）
constexpr int kPlotCols = 2;
constexpr int kPlotRows = 2;
} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    buildUi();
    setWindowTitle(QStringLiteral("QCustomPlot 多页 PDF 导出（Qt 5.15.2）"));
    resize(980, 760);
}

void MainWindow::buildUi()
{
    auto *central = new QWidget(this);
    auto *root    = new QVBoxLayout(central);

    // 预览：第 1 页的 4 个图（田字格）
    auto *grid = new QGridLayout;
    grid->setSpacing(8);
    m_plot00 = createSineCosPlot(0, 0);
    m_plot01 = createSineCosPlot(0, 1);
    m_plot10 = createSineCosPlot(0, 2);
    m_plot11 = createSineCosPlot(0, 3);
    grid->addWidget(m_plot00, 0, 0);
    grid->addWidget(m_plot01, 0, 1);
    grid->addWidget(m_plot10, 1, 0);
    grid->addWidget(m_plot11, 1, 1);
    root->addLayout(grid, 1);

    // 底栏：页数设置 + 进度条 + 导出按钮
    auto *bar = new QHBoxLayout;
    bar->addWidget(new QLabel(QStringLiteral("导出页数 N：")));
    m_pagesSpin = new QSpinBox;
    m_pagesSpin->setRange(1, 10000000); // 支持到 1000 万页
    m_pagesSpin->setValue(2); // 暂时 N=2，后续可直接在这里改
    m_pagesSpin->setMinimumWidth(140); // 加宽输入框，容纳 8 位以上数值
    m_pagesSpin->setToolTip(QStringLiteral("每页 4 个图（田字格），默认 2 页，可随时调整，最大 1000 万页"));
    bar->addWidget(m_pagesSpin);
    bar->addSpacing(12);

    // 导出进度条：导出期间显示，平时隐藏
    m_progress = new QProgressBar;
    m_progress->setRange(0, 100);
    m_progress->setValue(0);
    m_progress->setFormat(QStringLiteral("导出中 %p%"));
    m_progress->setMinimumWidth(220);
    m_progress->setVisible(false);
    bar->addWidget(m_progress, 1); // 占据剩余宽度
    bar->addStretch(0);

    m_btnExport = new QPushButton(QStringLiteral("导出 PDF"));
    connect(m_btnExport, &QPushButton::clicked, this, &MainWindow::onExportClicked);
    bar->addWidget(m_btnExport);
    root->addLayout(bar);

    setCentralWidget(central);
}

void MainWindow::onExportClicked()
{
    QString fileName = QFileDialog::getSaveFileName(
        this, QStringLiteral("保存 PDF"), QStringLiteral("plots.pdf"), QStringLiteral("PDF (*.pdf)"));
    if (fileName.isEmpty())
        return;
    if (QFileInfo(fileName).suffix().isEmpty())
        fileName += QStringLiteral(".pdf");

    exportToFile(fileName, m_pagesSpin->value());
}

void MainWindow::exportToFile(const QString &fileName, int pages)
{
    if (m_exporting)
        return; // 防止导出过程中重复点击
    m_exporting = true;
    m_exportingPages = pages;
    m_lastExportFile = fileName;
    m_lastProgressPct = -1;
    m_progressTimer.invalidate();
    m_btnExport->setEnabled(false);
    m_progress->setRange(0, pages * kPlotCols * kPlotRows); // 总格数：每页 4 个图
    m_progress->setValue(0);
    m_progress->setVisible(true);
    statusBar()->showMessage(QStringLiteral("正在导出 %1 页 PDF ... 0%").arg(pages));

    // 导出期间暂停 4 个预览图的重绘，避免它们和离屏渲染抢 CPU；
    // 进度条不属于这些控件，仍然正常刷新。
    m_plot00->setUpdatesEnabled(false);
    m_plot01->setUpdatesEnabled(false);
    m_plot10->setUpdatesEnabled(false);
    m_plot11->setUpdatesEnabled(false);

    // QCustomPlot 是 QWidget，QWidget 只能在 GUI 线程创建和绘制，
    // 因此导出必须在当前（GUI）线程执行，不能放进 QtConcurrent/QThread 工作线程
    // （否则会触发 "Widgets must be created in the GUI thread." 断言/崩溃）。
    // 为了让长导出期间界面仍能刷新，按时间片（约 30ms）processEvents，
    // 而不是每格都处理事件，减少无效的事件循环开销。
    QElapsedTimer timer;
    QElapsedTimer pumpTimer;
    timer.start();
    pumpTimer.start();
    QString err;
    const bool ok = PdfExporter::exportPages(
        fileName, pages, kPlotCols, kPlotRows,
        [](QCustomPlot *p, int page, int cell) { updateSineCosPlot(p, page, cell); },
        [this, &pumpTimer](int done, int total) {
            updateProgressUi(done, total);
            if (done >= total || pumpTimer.elapsed() >= 30)
            {
                // 只处理绘制/定时器等事件，排除鼠标键盘，避免导出期间重入点击。
                QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
                pumpTimer.restart();
            }
        },
        &err);

    finishExport(ok, timer.elapsed(), err);
}

void MainWindow::updateProgressUi(int done, int total)
{
    if (!m_progress || total <= 0)
        return;
    // 节流：同一百分比内且距上次更新不足 100ms 时跳过，避免大页数时高频重绘
    const int pct = int(double(done) * 100.0 / total);
    if (pct == m_lastProgressPct && m_progressTimer.isValid() && m_progressTimer.elapsed() < 100)
        return;
    m_lastProgressPct = pct;
    if (!m_progressTimer.isValid())
        m_progressTimer.start();

    // 范围在导出开始时按 pages*cols*rows 设好，这里只更新数值/文本。
    m_progress->setValue(done);
    m_progress->setFormat(QStringLiteral("导出中 %1%").arg(pct));
    statusBar()->showMessage(
        QStringLiteral("正在导出 %1 页 PDF ... %2%").arg(m_exportingPages).arg(pct));
}

int MainWindow::exportProgressValue() const
{
    return m_progress ? m_progress->value() : -1;
}

void MainWindow::finishExport(bool ok, qint64 elapsedMs, const QString &error)
{
    m_exporting = false;
    m_btnExport->setEnabled(true);
    m_progress->setValue(m_progress->maximum()); // 走满进度条
    m_progress->setVisible(false);
    m_lastError = error;

    // 恢复预览图重绘
    m_plot00->setUpdatesEnabled(true);
    m_plot01->setUpdatesEnabled(true);
    m_plot10->setUpdatesEnabled(true);
    m_plot11->setUpdatesEnabled(true);

    QString timeStr;
    if (elapsedMs >= 1000)
        timeStr = QStringLiteral("%1 秒").arg(elapsedMs / 1000.0, 0, 'f', 2);
    else
        timeStr = QStringLiteral("%1 毫秒").arg(elapsedMs);

    emit exportFinished(ok, elapsedMs);

    if (ok)
        statusBar()->showMessage(QStringLiteral("导出完成，耗时 %1").arg(timeStr), 10000);
    else
        statusBar()->showMessage(QStringLiteral("导出失败"), 10000);

    if (!m_showDialogs)
        return;

    if (ok)
        QMessageBox::information(
            this, QStringLiteral("导出完成"),
            QStringLiteral("已导出 %1 页 PDF：\n%2\n\n导出耗时：%3")
                .arg(m_pagesSpin->value()).arg(m_lastExportFile).arg(timeStr));
    else
        QMessageBox::critical(
            this, QStringLiteral("导出失败"),
            QStringLiteral("导出失败：%1").arg(m_lastExportFile));
}
