#include "mainwindow.h"

#include "pdfexporter.h"
#include "plotfactory.h"
#include "qcustomplot.h"

#include <QElapsedTimer>
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
#include <QtConcurrent/QtConcurrent>

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
    m_progress->setRange(0, pages * 4); // 总格数：每页 4 个图
    m_progress->setValue(0);
    m_progress->setVisible(true);
    statusBar()->showMessage(QStringLiteral("正在导出 %1 页 PDF ... 0%").arg(pages));

    // 异步导出：绘图与 PDF 渲染在 QtConcurrent 线程池线程执行，
    // 主线程（GUI）继续处理事件，界面不会被卡住。
    auto future = QtConcurrent::run([fileName, pages, this]() -> ExportResult {
        QElapsedTimer timer;
        timer.start();
        QString err;
        // 复用单 plot 渲染（updateSineCosPlot 首次调用自动完成初始化），
        // 进度按格回调，经 QueuedConnection 回主线程节流更新进度条。
        const bool ok = PdfExporter::exportPages(
            fileName, pages, 2, 2,
            [](QCustomPlot *p, int page, int cell) { updateSineCosPlot(p, page, cell); },
            [this](int done, int total) {
                QMetaObject::invokeMethod(
                    this, [this, done, total]() { updateProgressUi(done, total); },
                    Qt::QueuedConnection);
            },
            &err);
        return ExportResult{ ok, timer.elapsed(), err };
    });

    // 完成后回到主线程处理 UI（watcher 归属主线程，finished 信号在主线程发出）
    auto *watcher = new QFutureWatcher<ExportResult>(this);
    connect(watcher, &QFutureWatcher<ExportResult>::finished,
            this, [this, watcher]() { onExportFinished(watcher); });
    watcher->setFuture(future);
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

    m_progress->setRange(0, total);
    m_progress->setValue(done);
    m_progress->setFormat(QStringLiteral("导出中 %1%").arg(pct));
    statusBar()->showMessage(
        QStringLiteral("正在导出 %1 页 PDF ... %2%").arg(m_exportingPages).arg(pct));
}

int MainWindow::exportProgressValue() const
{
    return m_progress ? m_progress->value() : -1;
}

void MainWindow::onExportFinished(QFutureWatcher<ExportResult> *watcher)
{
    if (!watcher)
        return;
    const ExportResult result = watcher->result();
    watcher->deleteLater();

    m_exporting = false;
    m_btnExport->setEnabled(true);
    m_progress->setValue(m_progress->maximum()); // 走满进度条
    m_progress->setVisible(false);
    m_lastError = result.error;

    QString timeStr;
    if (result.elapsedMs >= 1000)
        timeStr = QStringLiteral("%1 秒").arg(result.elapsedMs / 1000.0, 0, 'f', 2);
    else
        timeStr = QStringLiteral("%1 毫秒").arg(result.elapsedMs);

    emit exportFinished(result.ok, result.elapsedMs);

    if (result.ok)
        statusBar()->showMessage(QStringLiteral("导出完成，耗时 %1").arg(timeStr), 10000);
    else
        statusBar()->showMessage(QStringLiteral("导出失败"), 10000);

    if (!m_showDialogs)
        return;

    if (result.ok)
        QMessageBox::information(
            this, QStringLiteral("导出完成"),
            QStringLiteral("已导出 %1 页 PDF：\n%2\n\n导出耗时：%3")
                .arg(m_pagesSpin->value()).arg(m_lastExportFile).arg(timeStr));
    else
        QMessageBox::critical(
            this, QStringLiteral("导出失败"),
            QStringLiteral("导出失败：%1").arg(m_lastExportFile));
}
