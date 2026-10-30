#ifndef PLOTFACTORY_H
#define PLOTFACTORY_H

#include <QVector>

class QCustomPlot;

// 创建第 page 页第 cell 格的全新图（GUI 预览用）：完整的独立 QCustomPlot 实例。
QCustomPlot *createSineCosPlot(int page, int cell, int pointCount = 100);

// 在已有 plot 上更新为第 page 页第 cell 格的数据与标题（PDF 导出的复用路径，用于提速）。
// 首次调用会自动完成静态配置（轴/图例/字体/两条曲线），之后只更新数据与标题文本。
void updateSineCosPlot(QCustomPlot *plot, int page, int cell, int pointCount = 100);

#endif // PLOTFACTORY_H
