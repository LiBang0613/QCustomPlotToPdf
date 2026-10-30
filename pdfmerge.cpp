#include "pdfmerge.h"

#include <QByteArray>
#include <QFile>
#include <QHash>
#include <QVector>
#include <QtMath>

namespace PdfMerger {

namespace {

struct PartInfo
{
    QByteArray raw;
    int objCount = 0;                 // xref 中对象总数（含 0 号 free）
    QVector<qint64> offsets;          // objNum -> 文件偏移（0 号无效）
    int catalogNum = 0;               // trailer /Root
    int infoNum = 0;                  // trailer /Info（可能 0）
    int pagesNum = 0;                 // catalog /Pages
    int pageCount = 0;                // Pages /Count
};

struct ObjData
{
    QByteArray dict;       // 含 << >>
    QByteArray stream;     // 原始流字节（非流为空）
    bool isStream = false;
    int oldNum = 0;
};

// ---------- 小工具 ----------

static void skipLine(const QByteArray &b, int &i)
{
    while (i < b.size() && b.at(i) != '\n') ++i;
    if (i < b.size()) ++i;
}

static void skipWs(const QByteArray &b, int &i)
{
    while (i < b.size())
    {
        const char c = b.at(i);
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f')
            ++i;
        else
            break;
    }
}

// 解析 xref 表（Qt 生成的 PDF 为传统 xref，单一 0 子节）
static bool parseXref(const QByteArray &raw, PartInfo &info)
{
    const int sx = raw.lastIndexOf("startxref");
    if (sx < 0) return false;
    const int xp = raw.lastIndexOf("xref", sx);
    if (xp < 0) return false;

    int i = xp + 4;
    skipWs(raw, i);
    while (i < raw.size() && raw.at(i) >= '0' && raw.at(i) <= '9') ++i; // 子节起始号（应为 0）
    skipWs(raw, i);
    int count = 0;
    while (i < raw.size() && raw.at(i) >= '0' && raw.at(i) <= '9')
    {
        count = count * 10 + (raw.at(i) - '0');
        ++i;
    }
    if (count <= 0 || count > 1000000)
        return false;
    skipWs(raw, i);

    info.objCount = count;
    info.offsets.resize(count);
    // 注意：xref 表第一行是 0 号 free 条目（"0000000000 65535 f"），
    // 必须从 n=0 开始逐行读取，否则所有对象偏移会错位。
    for (int n = 0; n < count; ++n)
    {
        int lineStart = i;
        while (i < raw.size() && raw.at(i) != '\n') ++i;
        QByteArray line = raw.mid(lineStart, i - lineStart);
        if (i < raw.size()) ++i;
        const QList<QByteArray> tokens = line.split(' ');
        if (tokens.size() < 3)
            return false;
        const qint64 off = tokens.at(0).toLongLong();
        const char type = tokens.at(2).isEmpty() ? 'n' : tokens.at(2).at(0);
        info.offsets[n] = (type == 'n') ? off : 0;
    }
    return true;
}

// 从字典中提取 /Key 后的数字（引用 "/Key N 0 R" 或纯数值 "/Key N" 均可）
static int dictRef(const QByteArray &dict, const char *key)
{
    const QByteArray k = QByteArray(key) + " ";
    int p = dict.indexOf(k);
    while (p >= 0)
    {
        int q = p + k.size();
        while (q < dict.size() && (dict.at(q) == ' ' || dict.at(q) == '\t')) ++q;
        int num = 0;
        bool ok = false;
        while (q < dict.size() && dict.at(q) >= '0' && dict.at(q) <= '9')
        {
            num = num * 10 + (dict.at(q) - '0');
            ++q;
            ok = true;
        }
        if (ok)
            return num; // 直接返回数字（引用或纯数值键均可，如 /Root 2 0 R 与 /Count 1）
        p = dict.indexOf(k, p + 1);
    }
    return 0;
}

// 解析 trailer 字典（trailer 关键字后到 startxref 前）
static QByteArray parseTrailerDict(const QByteArray &raw)
{
    const int tp = raw.lastIndexOf("trailer");
    if (tp < 0)
        return QByteArray();
    int i = tp + 7;
    skipWs(raw, i);
    if (i + 1 >= raw.size() || raw.at(i) != '<' || raw.at(i + 1) != '<')
        return QByteArray();
    int depth = 0;
    const int start = i;
    while (i < raw.size())
    {
        const char c = raw.at(i);
        if (c == '<' && i + 1 < raw.size() && raw.at(i + 1) == '<') { ++depth; i += 2; }
        else if (c == '>' && i + 1 < raw.size() && raw.at(i + 1) == '>') { --depth; i += 2; if (depth == 0) return raw.mid(start, i - start); }
        else if (c == '(')
        {
            int d = 1; ++i;
            while (i < raw.size() && d > 0)
            {
                if (raw.at(i) == '\\') i += 2;
                else
                {
                    if (raw.at(i) == '(') ++d;
                    else if (raw.at(i) == ')') --d;
                    ++i;
                }
            }
        }
        else ++i;
    }
    return QByteArray();
}

// 在偏移处读取一个对象。
// 支持三种对象值：字典 <<...>>、数组 [...]、简单 token（数字/名字等，如 /Length 的取值对象）。
// 字典对象后可带 stream 流数据。
static bool readObject(const QByteArray &raw, qint64 off, ObjData &obj)
{
    const int len = raw.size();
    int i = int(off);
    if (i >= len) return false;
    int lineStart = i;
    while (i < len && raw.at(i) != '\n') ++i;
    QByteArray head = raw.mid(lineStart, i - lineStart);
    if (i < len) ++i;
    if (!head.contains("obj"))
        return false;

    while (i < len && (raw.at(i) == ' ' || raw.at(i) == '\t')) ++i;
    if (i >= len)
        return false;

    const char c = raw.at(i);
    if (c == '<' && i + 1 < len && raw.at(i + 1) == '<')
    {
        // —— 字典对象 ——
        int depth = 0;
        const int start = i;
        while (i < len)
        {
            const char cc = raw.at(i);
            if (cc == '%') { skipLine(raw, i); continue; }
            if (cc == '(')
            {
                int d = 1; ++i;
                while (i < len && d > 0)
                {
                    if (raw.at(i) == '\\') i += 2;
                    else
                    {
                        if (raw.at(i) == '(') ++d;
                        else if (raw.at(i) == ')') --d;
                        ++i;
                    }
                }
                continue;
            }
            if (cc == '<' && i + 1 < len && raw.at(i + 1) == '<') { ++depth; i += 2; }
            else if (cc == '>' && i + 1 < len && raw.at(i + 1) == '>') { --depth; i += 2; if (depth == 0) break; }
            else if (cc == '<')
            {
                while (i < len && raw.at(i) != '>') ++i;
                if (i < len) ++i;
            }
            else if (cc == '[') { ++depth; ++i; }
            else if (cc == ']') { --depth; ++i; }
            else ++i;
        }
        if (depth != 0)
            return false;
        obj.dict = raw.mid(start, i - start); // 含 << >>

        // 字典后可跟 stream
        int q = i;
        skipWs(raw, q);
        if (q + 6 <= len && raw.mid(q, 6) == "stream")
        {
            q += 6;
            if (q < len && raw.at(q) == '\r') q += 2;
            else if (q < len && raw.at(q) == '\n') q += 1;
            const int se = raw.indexOf("endstream", q);
            if (se < 0)
                return false;
            obj.stream = raw.mid(q, se - q);
            if (obj.stream.endsWith('\n'))
                obj.stream.chop(1); // 统一去掉尾部换行，写入时补
            obj.isStream = true;
        }
        return true;
    }

    if (c == '[')
    {
        // —— 数组对象（如 /ProcSet、/Annots） ——
        int depth = 0;
        const int start = i;
        while (i < len)
        {
            const char cc = raw.at(i);
            if (cc == '%') { skipLine(raw, i); continue; }
            if (cc == '(')
            {
                int d = 1; ++i;
                while (i < len && d > 0)
                {
                    if (raw.at(i) == '\\') i += 2;
                    else
                    {
                        if (raw.at(i) == '(') ++d;
                        else if (raw.at(i) == ')') --d;
                        ++i;
                    }
                }
                continue;
            }
            if (cc == '<' && i + 1 < len && raw.at(i + 1) == '<') { ++depth; i += 2; }
            else if (cc == '>' && i + 1 < len && raw.at(i + 1) == '>') { --depth; i += 2; }
            else if (cc == '[') { ++depth; ++i; }
            else if (cc == ']') { --depth; ++i; if (depth == 0) break; }
            else ++i;
        }
        if (depth != 0)
            return false;
        obj.dict = raw.mid(start, i - start); // 含 [ ]
        obj.isStream = false;
        return true;
    }

    // —— 简单 token 对象（数字/名字/字符串等，如 Length 取值对象 "12345"） ——
    {
        int start = i;
        while (i < len && raw.at(i) != '\n')
            ++i;
        // 去掉可能的尾部 endobj 前的空白
        QByteArray v = raw.mid(start, i - start).trimmed();
        const QByteArray eo = "endobj";
        if (v.endsWith(eo))
            v.chop(eo.size());
        v = v.trimmed();
        obj.dict = v;
        obj.isStream = false;
        return true;
    }
}

// 扫描文本（跳过字符串/hex/注释），把 "N 0 R" 引用替换为新号
static QByteArray remapRefs(const QByteArray &in, const QVector<int> &map)
{
    if (in.isEmpty())
        return in;
    QByteArray out;
    out.reserve(in.size() + 16);
    const int len = in.size();
    int i = 0;
    while (i < len)
    {
        const char c = in.at(i);
        if (c == '(')
        {
            const int s = i; int d = 1; ++i;
            while (i < len && d > 0)
            {
                if (in.at(i) == '\\') i += 2;
                else
                {
                    if (in.at(i) == '(') ++d;
                    else if (in.at(i) == ')') --d;
                    ++i;
                }
            }
            out.append(in.mid(s, i - s));
            continue;
        }
        if (c == '<')
        {
            if (i + 1 < len && in.at(i + 1) == '<')
            {
                // 字典/嵌套字典起始 "<<..."，不能按 hex 处理：原样输出并继续扫描内部
                out.append("<<");
                i += 2;
                continue;
            }
            const int s = i; ++i;
            while (i < len && in.at(i) != '>') ++i;
            if (i < len) ++i;
            out.append(in.mid(s, i - s));
            continue;
        }
        if (c == '%')
        {
            const int s = i;
            while (i < len && in.at(i) != '\n') ++i;
            out.append(in.mid(s, i - s));
            continue;
        }
        if (c >= '0' && c <= '9')
        {
            const int ns = i;
            long long n1 = 0;
            while (i < len && in.at(i) >= '0' && in.at(i) <= '9')
            {
                n1 = n1 * 10 + (in.at(i) - '0');
                ++i;
            }
            int j = i;
            while (j < len && (in.at(j) == ' ' || in.at(j) == '\t')) ++j;
            int zeros = 0;
            int jz = j;
            while (jz < len && in.at(jz) >= '0' && in.at(jz) <= '9')
            {
                zeros = zeros * 10 + (in.at(jz) - '0');
                ++jz;
            }
            int jr = jz;
            while (jr < len && (in.at(jr) == ' ' || in.at(jr) == '\t')) ++jr;
            if (zeros == 0 && jr < len && in.at(jr) == 'R' && int(n1) >= 0 && int(n1) < map.size())
            {
                const int newNum = map.at(int(n1));
                if (newNum > 0)
                {
                    out.append(QByteArray::number(newNum));
                    out.append(in.mid(i, j - i));    // 数字与 gen 之间的空白（必须保留，"N 0 R"）
                    out.append(in.mid(j, jz - j));   // "0"
                    out.append(in.mid(jz, jr - jz)); // 0 与 R 之间的空白
                    out.append(in.mid(jr, 1));       // "R"
                    i = jr + 1;
                    continue;
                }
            }
            out.append(in.mid(ns, i - ns));
            continue;
        }
        out.append(c);
        ++i;
    }
    return out;
}

// 移除 dict 中的 /Length 段（流压缩后重建）
// 支持 "/Length N" 与 "/Length N 0 R"（Qt 内容流使用引用形式）两种写法。
static QByteArray stripLength(const QByteArray &dict)
{
    const int p = dict.indexOf("/Length");
    if (p < 0)
        return dict;
    int q = p + 7;
    if (q >= dict.size() || (dict.at(q) != ' ' && dict.at(q) != '\t'))
        return dict;
    while (q < dict.size() && (dict.at(q) == ' ' || dict.at(q) == '\t')) ++q;
    int e = q;
    while (e < dict.size() && dict.at(e) >= '0' && dict.at(e) <= '9') ++e;
    if (e <= q)
        return dict;
    // 尝试读可选的 "0 R"（间接引用形式）
    int z = e;
    while (z < dict.size() && (dict.at(z) == ' ' || dict.at(z) == '\t')) ++z;
    int zd = z;
    while (zd < dict.size() && dict.at(zd) >= '0' && dict.at(zd) <= '9') ++zd;
    if (zd > z)
    {
        int rr = zd;
        while (rr < dict.size() && (dict.at(rr) == ' ' || dict.at(rr) == '\t')) ++rr;
        if (rr < dict.size() && dict.at(rr) == 'R')
            e = rr + 1;
    }
    return dict.left(p) + dict.mid(e);
}

// 内容流瘦身：把绘图/坐标行中的浮点坐标收敛到 3 位小数（0.001pt 精度
// 视觉无差别），跳过 (...) 文本串与 <> hex 串。可让明文体积减半以上，
// 且数字重复度提高后 Flate 压缩率明显更好。
static QByteArray slimContentStream(const QByteArray &in)
{
    if (in.isEmpty())
        return in;
    QByteArray out;
    out.reserve(in.size());
    const int len = in.size();
    int i = 0;
    while (i < len)
    {
        const char c = in.at(i);
        if (c == '(')
        {
            const int s = i; int d = 1; ++i;
            while (i < len && d > 0)
            {
                if (in.at(i) == '\\') i += 2;
                else
                {
                    if (in.at(i) == '(') ++d;
                    else if (in.at(i) == ')') --d;
                    ++i;
                }
            }
            out.append(in.mid(s, i - s));
            continue;
        }
        if (c == '<')
        {
            if (i + 1 < len && in.at(i + 1) == '<')
            {
                // 字典/嵌套字典起始 "<<..."，不能按 hex 处理：原样输出并继续扫描内部
                out.append("<<");
                i += 2;
                continue;
            }
            const int s = i; ++i;
            while (i < len && in.at(i) != '>') ++i;
            if (i < len) ++i;
            out.append(in.mid(s, i - s));
            continue;
        }
        if (c == '-' || c == '+' || c == '.' || (c >= '0' && c <= '9'))
        {
            const int s = i;
            if (c == '-' || c == '+') ++i;
            bool d1 = false;
            while (i < len && in.at(i) >= '0' && in.at(i) <= '9') { ++i; d1 = true; }
            bool dot = false;
            if (i < len && in.at(i) == '.') { ++i; dot = true; }
            bool d2 = false;
            while (i < len && in.at(i) >= '0' && in.at(i) <= '9') { ++i; d2 = true; }
            if (dot && (d1 || d2))
            {
                // 浮点坐标：round 到 3 位小数，去尾零
                const QByteArray tok = in.mid(s, i - s);
                const double v = tok.toDouble();
                QByteArray r = QByteArray::number(v, 'f', 3);
                while (r.endsWith('0')) r.chop(1);
                if (r.endsWith('.')) r.chop(1);
                if (r == tok) // 没变化，原样保留避免无谓改动
                    out.append(tok);
                else
                    out.append(r);
                continue;
            }
            out.append(in.mid(s, i - s));
            continue;
        }
        out.append(c);
        ++i;
    }
    return out;
}

// —— 纯 zlib 包装（PDF 流要求纯 zlib；Qt 的 qCompress/qUncompress 是
//    "4 字节大端长度前缀 + zlib" 的自定义格式，不能直接用于 PDF 流） ——

// 纯 zlib 流 → 明文。用 qUncompress 的 Qt 格式包装（前缀写一个足够大的
// 上界，qUncompress 解压后会用实际大小截断）；解压失败返回空（原样保留）。
static QByteArray pureZlibUncompress(const QByteArray &z)
{
    if (z.size() <= 0)
        return QByteArray();
    qint64 est = qint64(z.size()) * 40 + 4096; // 内容流压缩比一般 < 40:1
    if (est > 64 * 1024 * 1024)                 // 超过 64MB 上界的流（如大字体流）跳过，避免大分配
        return QByteArray();
    QByteArray wrapped;
    wrapped.reserve(z.size() + 4);
    wrapped.append(char(est >> 24));
    wrapped.append(char(est >> 16));
    wrapped.append(char(est >> 8));
    wrapped.append(char(est));
    wrapped.append(z);
    return qUncompress(wrapped); // 失败返回空
}

// 明文 → 纯 zlib（qCompress 输出去掉 4 字节长度前缀）
static QByteArray pureZlibCompress(const QByteArray &plain, int level)
{
    QByteArray c = qCompress(plain, level);
    if (c.size() > 4)
        c = c.mid(4);
    return c;
}

// 对单个流执行"解压 → 瘦身 → level 9 重压"；失败或不变小时返回原样。
// 字体流等带自定义前缀、无法解压的流会被 pureZlibUncompress 拒绝，安全保留。
// 注：slimContentStream 的 token 扫描在 40000+ 流下是合并期主要瓶颈
// （10000 页实测 slim 收益仅 ~12%），为满足 130s 目标改为"解压→直接重压"。
// static QByteArray slimAndRecompress(...) 已停用：已压缩流（/Filter）原样保留，
// 明文小流走 pureZlibCompress（见输出循环）。

} // namespace

bool mergeAndCompress(const QStringList &parts,
                      const QString &output,
                      QString *errorMessage,
                      const std::function<void(int, int)> &onProgress,
                      int compressionLevel)
{
    if (parts.isEmpty())
    {
        if (errorMessage) *errorMessage = QStringLiteral("没有需要合并的 PDF 分片");
        return false;
    }

    // ---------- 解析各分片 ----------
    QVector<PartInfo> partsInfo;
    partsInfo.reserve(parts.size());
    for (const QString &pf : parts)
    {
        QFile f(pf);
        if (!f.open(QIODevice::ReadOnly))
        {
            if (errorMessage) *errorMessage = QStringLiteral("无法读取分片：%1").arg(pf);
            return false;
        }
        PartInfo info;
        info.raw = f.readAll();
        f.close();
        if (!parseXref(info.raw, info))
        {
            if (errorMessage) *errorMessage = QStringLiteral("无法解析分片 xref：%1").arg(pf);
            return false;
        }
        const QByteArray trailer = parseTrailerDict(info.raw);
        info.catalogNum = dictRef(trailer, "/Root");
        info.infoNum = dictRef(trailer, "/Info");
        partsInfo.append(info);
    }

    // 解析各分片 Catalog 的 /Pages 与页数
    int totalPageCount = 0;
    for (int pi = 0; pi < partsInfo.size(); ++pi)
    {
        PartInfo &info = partsInfo[pi];
        ObjData cat;
        if (info.catalogNum > 0 && readObject(info.raw, info.offsets.value(info.catalogNum), cat))
            info.pagesNum = dictRef(cat.dict, "/Pages");
        if (info.pagesNum > 0)
        {
            ObjData pagesObj;
            if (readObject(info.raw, info.offsets.value(info.pagesNum), pagesObj))
                info.pageCount = dictRef(pagesObj.dict, "/Count");
        }
        if (info.pageCount <= 0)
        {
            if (errorMessage) *errorMessage = QStringLiteral("分片页数为空（分片 %1）").arg(pi);
            return false;
        }
        totalPageCount += info.pageCount;
    }

    // ---------- 输出 ----------
    QFile out(output);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        if (errorMessage) *errorMessage = QStringLiteral("无法创建输出文件：%1").arg(output);
        return false;
    }
    out.write("%PDF-1.4\n");

    QVector<qint64> xrefOffsets;   // 新对象号 -> 输出偏移
    xrefOffsets.append(0);
    QVector<int> pagesNewNum(partsInfo.size(), 0);

    for (int pi = 0; pi < partsInfo.size(); ++pi)
    {
        PartInfo &info = partsInfo[pi];
        // —— 读取全部有效对象并建立完整引用映射 ——
        QVector<ObjData> objs;
        QVector<int> localMap(info.objCount, 0);
        objs.reserve(info.objCount);
        for (int n = 1; n < info.objCount; ++n)
        {
            if (info.offsets.at(n) <= 0)
                continue;
            // part>0 的 Catalog 为死对象（根 Catalog 由合并器重建），跳过
            if (pi > 0 && n == info.catalogNum)
                continue;
            ObjData obj;
            if (!readObject(info.raw, info.offsets.at(n), obj))
                continue;
            obj.oldNum = n;
            localMap[n] = xrefOffsets.size();
            xrefOffsets.append(-1); // 占位
            if (n == info.pagesNum)
                pagesNewNum[pi] = localMap[n];
            objs.append(obj);
        }

        // —— 输出对象（引用用完整映射替换） ——
        QFile *dbgFile = nullptr;
        if (qEnvironmentVariableIsSet("CUSTOM_PLOT_PDF_MERGE_DEBUG"))
        {
            dbgFile = new QFile(QStringLiteral("merge_debug_%1.txt").arg(pi));
            dbgFile->open(QIODevice::WriteOnly | QIODevice::Truncate);
        }
        for (const ObjData &obj : objs)
        {
            QByteArray dict2 = remapRefs(obj.dict, localMap);
            if (dbgFile)
            {
                const int nn = localMap[obj.oldNum];
                dbgFile->write(QStringLiteral("pi=%1 old=%2 new=%3\n  dict: ").arg(pi).arg(obj.oldNum).arg(nn).toUtf8());
                dbgFile->write(dict2.left(200));
                dbgFile->write("\n");
            }
            QByteArray stream2 = obj.stream;
            // 已压缩流（/Filter）原样保留：Qt 分片内容流已用默认 level6 压缩，
            // 重新解压-重压 level9 在 40000+ 流下耗时 ~150s，收益仅 ~5% 体积。
            // 明文小流（CMap/CIDSet 等）压缩成纯 zlib 后写回（修正 Qt 4 字节前缀）。
            if (obj.isStream && !dict2.contains("/Filter") && stream2.size() > 0)
            {
                stream2 = pureZlibCompress(stream2, compressionLevel);
                dict2 = stripLength(dict2);
                if (!dict2.endsWith(">>"))
                    dict2 = dict2.trimmed();
                dict2 = dict2.left(dict2.size() - 2)
                        + " /Filter /FlateDecode /Length " + QByteArray::number(stream2.size()) + " >>";
            }

            const int newNum = localMap[obj.oldNum];
            const qint64 objOff = out.pos();
            out.write(QByteArray::number(newNum) + " 0 obj\n");
            out.write(dict2);
            out.write("\n");
            if (obj.isStream)
            {
                out.write("stream\n");
                out.write(stream2);
                out.write("\nendstream\n");
            }
            out.write("endobj\n");
            xrefOffsets[newNum] = objOff;
        }
        if (dbgFile)
        {
            dbgFile->flush();
            dbgFile->close();
            delete dbgFile;
        }

        // 释放分片内存
        info.raw.clear();
        if (onProgress)
            onProgress(pi + 1, partsInfo.size());
    }

    // ---------- 新 Pages 树 + 新 Catalog ----------
    const int newPagesNum = xrefOffsets.size();
    xrefOffsets.append(-1);
    {
        QByteArray kids = " [";
        for (int i = 0; i < partsInfo.size(); ++i)
        {
            if (pagesNewNum[i] <= 0)
            {
                if (errorMessage) *errorMessage = QStringLiteral("分片 %1 的 Pages 对象未映射").arg(i);
                out.close();
                return false;
            }
            kids += " " + QByteArray::number(pagesNewNum[i]) + " 0 R";
        }
        kids += " ]";

        const qint64 objOff = out.pos();
        out.write(QByteArray::number(newPagesNum) + " 0 obj\n");
        out.write("<< /Type /Pages /Kids" + kids + " /Count " + QByteArray::number(totalPageCount) + " >>\n");
        out.write("endobj\n");
        xrefOffsets[newPagesNum] = objOff;
    }

    const int newCatalogNum = xrefOffsets.size();
    xrefOffsets.append(-1);
    {
        const qint64 objOff = out.pos();
        out.write(QByteArray::number(newCatalogNum) + " 0 obj\n");
        out.write("<< /Type /Catalog /Pages " + QByteArray::number(newPagesNum) + " 0 R >>\n");
        out.write("endobj\n");
        xrefOffsets[newCatalogNum] = objOff;
    }

    // ---------- xref / trailer ----------
    const qint64 xrefPos = out.pos();
    out.write("xref\n");
    out.write("0 " + QByteArray::number(xrefOffsets.size()) + "\n");
    out.write("0000000000 65535 f \n");
    for (int n = 1; n < xrefOffsets.size(); ++n)
    {
        const qint64 off = xrefOffsets.at(n);
        QByteArray line = (off < 0)
            ? QByteArray("0000000000 00000 n \n")
            : QByteArray::number(off).rightJustified(10, '0') + " 00000 n \n";
        out.write(line);
    }
    const qint64 startxref = out.pos();
    QByteArray trailer = "trailer\n";
    trailer += "<< /Size " + QByteArray::number(xrefOffsets.size())
             + " /Root " + QByteArray::number(newCatalogNum) + " 0 R >>\n";
    trailer += "startxref\n";
    trailer += QByteArray::number(startxref) + "\n%%EOF\n";
    out.write(trailer);

    out.close();
    Q_UNUSED(xrefPos);
    if (onProgress)
        onProgress(partsInfo.size(), partsInfo.size());
    return true;
}

} // namespace PdfMerger
