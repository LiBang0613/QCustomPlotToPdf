#include "plotfactory.h"

#include "qcustomplot.h"

#include <QRandomGenerator>
#include <QtMath>

// 静态配置：轴、图例、字体、两条曲线与标题占位（只做一次，见 updateSineCosPlot）
static void setupSineCosPlot(QCustomPlot *plot)
{
    // —— 坐标轴与图例 ——
    // 注意：Qt 5.15 的 QPdfWriter 嵌入 TTC 集合字体（如微软雅黑 msyh.ttc）时
    // 会写出损坏的字体名，导致 PDF 查看器中文字形乱码；SimHei（simhei.ttf）
    // 为单 TTF 文件，可被正确嵌入，因此这里统一使用 SimHei。
    plot->xAxis->setLabel(QStringLiteral("x"));
    plot->yAxis->setLabel(QStringLiteral("y"));
    plot->xAxis->setRange(0.0, 4.0 * M_PI);
    plot->yAxis->setRange(-1.6, 1.6);
    plot->axisRect()->setupFullAxesBox();                    // 四边都显示刻度
    plot->setBackground(Qt::white);                          // PDF 白底

    // —— 网格线：QCustomPlot 默认 Qt::DotLine 虚线，Qt PDF 引擎会把每段虚线
    //    输出为大量细矩形 fill（每页数万条 lineto，是导出 PDF 体积爆炸的主因）；
    //    改为细实线网格，视觉几乎无差别，体积可降一个数量级 ——
    const QPen gridPen(QColor(200, 200, 200), 0, Qt::SolidLine);
    plot->xAxis->grid()->setPen(gridPen);
    plot->yAxis->grid()->setPen(gridPen);
    plot->xAxis2->grid()->setPen(gridPen);
    plot->yAxis2->grid()->setPen(gridPen);
    plot->xAxis2->grid()->setVisible(false);                 // 顶部/右侧网格默认隐藏，保持默认观感
    plot->yAxis2->grid()->setVisible(false);

    plot->legend->setVisible(true);
    plot->legend->setFont(QFont(QStringLiteral("SimHei"), 8));

    // —— 两条曲线：正弦+噪声 / 余弦+噪声 ——
    plot->addGraph();
    plot->graph(0)->setPen(QPen(QColor(31, 119, 180), 2));   // 蓝
    plot->graph(0)->setName(QStringLiteral("正弦+噪声"));
    plot->addGraph();
    plot->graph(1)->setPen(QPen(QColor(214, 39, 40), 2));    // 红
    plot->graph(1)->setName(QStringLiteral("余弦+噪声"));

    // —— 标题占位（文本每次由 update 设置；字体用 TTF 的 SimHei，原因见上） ——
    plot->plotLayout()->insertRow(0);
    plot->plotLayout()->addElement(0, 0, new QCPTextElement(
        plot, QString(),
        QFont(QStringLiteral("SimHei"), 10, QFont::Bold)));
}

void updateSineCosPlot(QCustomPlot *plot, int page, int cell, int pointCount)
{
    if (!plot)
        return;

    // 首次调用先完成静态配置（之后只更新数据与标题，大幅降低重复初始化开销）
    if (plot->graphCount() == 0)
        setupSineCosPlot(plot);

    // 每个格子使用不同的频率/相位，让田字格中的 4 个图可区分；
    // 噪声种子由 page/cell 决定，保证多次导出结果一致（确定性）。
    static const double freq[4]  = { 1.0, 1.6, 2.0, 0.7 };
    static const double phase[4] = { 0.0, 0.4, 1.1, 1.6 };
    const int idx = ((cell % 4) + 4) % 4;
    const double f  = freq[idx];
    const double ph = phase[idx];

    // —— 数据：两条曲线，各 pointCount 个点（默认 100），正弦/余弦加噪声 ——
    QVector<double> x(pointCount), ySin(pointCount), yCos(pointCount);
    QRandomGenerator rng(quint32(0x4351u + page * 101u + cell * 7u));
    for (int i = 0; i < pointCount; ++i)
    {
        const double t = i / double(pointCount - 1) * 4.0 * M_PI; // 0 ~ 4π，两个周期
        const double noiseSin = (rng.generateDouble() - 0.5) * 0.3; // ±0.15
        const double noiseCos = (rng.generateDouble() - 0.5) * 0.3;
        x[i]     = t;
        ySin[i]  = std::sin(f * t + ph) + noiseSin;
        yCos[i]  = std::cos(f * t + ph) + noiseCos;
    }

    // 直接替换数据容器（data()->set），不走 QCPGraph::setData：
    // setData 内部会触发 replot()，而复用图用于离屏矢量导出、无需重绘事件，
    // 跳过 replot 可省掉每次的布局重算（导出提速）。
    QVector<QCPGraphData> sinData(pointCount), cosData(pointCount);
    for (int i = 0; i < pointCount; ++i)
    {
        sinData[i] = QCPGraphData(x[i], ySin[i]);
        cosData[i] = QCPGraphData(x[i], yCos[i]);
    }
    plot->graph(0)->data()->set(sinData, true); // x 递增，已排序
    plot->graph(1)->data()->set(cosData, true);

    // —— 标题：第几页第几图 ——
    if (auto *title = qobject_cast<QCPTextElement *>(plot->plotLayout()->element(0, 0)))
        title->setText(QStringLiteral("第 %1 页 · 第 %2 图").arg(page + 1).arg(cell + 1));
}

QCustomPlot *createSineCosPlot(int page, int cell, int pointCount)
{
    QCustomPlot *plot = new QCustomPlot();
    updateSineCosPlot(plot, page, cell, pointCount);
    plot->replot();
    return plot;
}
