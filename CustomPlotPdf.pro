# ============================================================
#  CustomPlotPdf.pro
#  QCustomPlot 多页 PDF 导出示例（Qt 5.15.2 / C++ / qmake）
#
#  功能：
#    1. 使用 QCustomPlot 绘图（每图两条曲线：正弦+噪声 / 余弦+噪声，各 100 点）
#    2. 每页 4 个图，田字格（2 行 x 2 列）
#    3. 导出 N 页 PDF（默认 N=2，可通过 GUI 微调框 / 命令行 --pages 设置）
#    4. 通过 QPdfWriter + QCPPainter，用 QCustomPlot::toPainter 矢量绘制到 PDF
# ============================================================

QT       += core gui printsupport widgets

CONFIG   += c++11

TARGET   = CustomPlotPdf
TEMPLATE = app

# QCustomPlot 源码直接放在项目目录中参与编译
SOURCES += \
    main.cpp \
    mainwindow.cpp \
    pdfexporter.cpp \
    plotfactory.cpp \
    qcustomplot.cpp

HEADERS += \
    mainwindow.h \
    pdfexporter.h \
    plotfactory.h \
    qcustomplot.h

# 多进程分片合并方案（当前单进程导出未使用）保留源码，需要时再追加：
#   SOURCES += pdfmerge.cpp
#   HEADERS += pdfmerge.h

INCLUDEPATH += $$PWD

# 源码中带有中文字符串字面量：MSVC 需要 /utf-8 才能正确解析 UTF-8 源码
msvc: QMAKE_CXXFLAGS += /utf-8

CONFIG(release, debug|release): DEFINES += QT_NO_DEBUG_OUTPUT
