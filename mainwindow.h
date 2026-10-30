#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QString>
#include <QFutureWatcher>
#include <QElapsedTimer>

// 一次异步导出的结果：是否成功 + 导出耗时（毫秒）+ 失败原因
struct ExportResult
{
    bool ok;
    qint64 elapsedMs;
    QString error;
};

class QCustomPlot;
class QProgressBar;
class QPushButton;
class QSpinBox;

// 主窗口：预览第 1 页的 4 个图（田字格），可选择导出页数 N 并导出 PDF。
// 导出在 QtConcurrent 线程池中异步执行，不阻塞界面；完成后弹窗显示耗时。
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override = default;

    // 对外入口：异步导出到 fileName（不阻塞界面），完成后发 exportFinished 信号。
    // pages 为导出页数 N。可用于自动化/自测场景。
    void exportToFile(const QString &fileName, int pages);

    // 是否弹提示框（自测模式下关闭）
    void setShowDialogs(bool show) { m_showDialogs = show; }

    // 当前进度条值（自测用；-1 表示无进度条）
    int exportProgressValue() const;

    // 最近一次导出的失败原因（成功时为空；自测用）
    QString lastExportError() const { return m_lastError; }

signals:
    // 导出完成（ok：是否成功；elapsedMs：耗时毫秒）
    void exportFinished(bool ok, qint64 elapsedMs);

private slots:
    void onExportClicked();

private:
    void onExportFinished(QFutureWatcher<ExportResult> *watcher);
    void updateProgressUi(int done, int total);
    void buildUi();

    QCustomPlot *m_plot00 = nullptr;
    QCustomPlot *m_plot01 = nullptr;
    QCustomPlot *m_plot10 = nullptr;
    QCustomPlot *m_plot11 = nullptr;
    QSpinBox    *m_pagesSpin = nullptr;
    QPushButton *m_btnExport = nullptr;
    QProgressBar *m_progress = nullptr;
    QString      m_lastExportFile;
    QString      m_lastError;
    QElapsedTimer m_progressTimer;
    int m_lastProgressPct = -1;
    int m_exportingPages = 0;
    bool m_exporting = false;
    bool m_showDialogs = true;
};

#endif // MAINWINDOW_H
