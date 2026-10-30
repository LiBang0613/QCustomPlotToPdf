#ifndef PDFMERGE_H
#define PDFMERGE_H

#include <QString>
#include <QStringList>
#include <functional>

namespace PdfMerger
{
    // 将多个 Qt 5 QPdfWriter 生成的 PDF 分片合并为一个 PDF：
    //   - 对象重编号 + 内容哈希去重（相同内容的对象只保留一份，例如嵌入字体）
    //   - 对未压缩的内容流 / 字体流做 Flate 压缩（qCompress，zlib 流，符合 /FlateDecode）
    //   - 重写 Pages 树与 xref/trailer
    // onProgress(done, total)：按已处理字节回调（合并器内部自用，可为空）。
    // 返回 true 表示成功；失败时通过 errorMessage 返回原因。
    bool mergeAndCompress(const QStringList &parts,
                          const QString &output,
                          QString *errorMessage = nullptr,
                          const std::function<void(int done, int total)> &onProgress = nullptr,
                          int compressionLevel = 6);
}

#endif // PDFMERGE_H
