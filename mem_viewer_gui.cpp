#include "mem_viewer_gui.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QColorDialog>
#include <QComboBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QStyle>
#include <QStyleOptionSlider>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>
#include <QPainter>
#include <QFont>
#include <QFontMetrics>
#include <QFileDialog>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMouseEvent>
#include <QKeySequence>
#include <QSignalBlocker>
#include <QTextEdit>
#include <QTextBrowser>
#include <QUrl>
#include <QWheelEvent>
#include <QResizeEvent>
#include <QMainWindow>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <functional>
#include <map>
#include <limits>
#include <set>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include <sys/prctl.h>
#include <sys/uio.h>
#include <unistd.h>

namespace {

static bool mem_viewer_debug_enabled() {
    static const bool enabled = []() {
        const char *value = std::getenv("MEM_VIEWER_DEBUG");
        return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0;
    }();
    return enabled;
}

static void mem_viewer_debug_log(const char *fmt, ...) {
    if (!mem_viewer_debug_enabled()) {
        return;
    }

    std::fprintf(stderr, "[mem_viewer_gui pid=%ld] ", static_cast<long>(getpid()));
    va_list args;
    va_start(args, fmt);
    std::vfprintf(stderr, fmt, args);
    va_end(args);
    std::fprintf(stderr, "\n");
    std::fflush(stderr);
}

static bool mem_viewer_auto_refresh_forced_off() {
    static const bool disabled = []() {
        const char *value = std::getenv("MEM_VIEWER_DISABLE_AUTO_REFRESH");
        return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0;
    }();
    return disabled;
}

static bool mem_viewer_static_file_mode() {
    static const bool enabled = []() {
        const char *value = std::getenv("MEM_VIEWER_STATIC_FILE");
        return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0;
    }();
    return enabled;
}

static double mem_viewer_now_seconds() {
    using clock = std::chrono::steady_clock;
    const auto now = clock::now().time_since_epoch();
    return std::chrono::duration<double>(now).count();
}

constexpr size_t kBytesPerRow = 16;
constexpr int kRefreshMs = 100;
constexpr double kFadeSeconds = 1.6;
constexpr size_t kSearchChunkBytes = 64 * 1024;
constexpr double kAddressX = 10.0;
constexpr double kAddressChars = 8.0;
constexpr double kAddressCharWidth = 7.0;
constexpr double kGapAddressToHex = 6.0;
constexpr double kGapHexToAscii = 6.0;

static const QColor kDefaultAnnotationColor(0x72, 0xE6, 0x7A);

struct AnnotationEntry {
    std::vector<size_t> positions;
    std::string note;
    QColor color = kDefaultAnnotationColor;
};

struct AnnotationColorPoint {
    size_t position = 0;
    QRgb color = 0U;
};

class NotePreview : public QTextBrowser {
public:
    explicit NotePreview(QWidget *parent = nullptr)
        : QTextBrowser(parent) {
        setOpenLinks(false);
        setOpenExternalLinks(false);
        setReadOnly(true);
    }

    std::function<void(const QUrl&)> onLinkActivated;

protected:
    void mouseReleaseEvent(QMouseEvent *event) override {
        if(event != nullptr && event->button() == Qt::LeftButton && (event->modifiers() & Qt::ControlModifier) != 0) {
            const QString anchor = anchorAt(event->position().toPoint());
            if(!anchor.isEmpty()) {
                if(onLinkActivated) {
                    onLinkActivated(QUrl(anchor));
                }
                event->accept();
                return;
            }
        }
        QTextBrowser::mouseReleaseEvent(event);
    }
};

enum class SearchFormat {
    Hex,
    Decimal,
};

enum class EndianMode {
    Little,
    Big,
};

static std::string trim_copy(const std::string &input) {
    size_t begin = 0;
    size_t end = input.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(input[begin])) != 0) {
        ++begin;
    }
    while (end > begin && std::isspace(static_cast<unsigned char>(input[end - 1])) != 0) {
        --end;
    }
    return input.substr(begin, end - begin);
}

static bool parse_byte_value(const std::string &text, uint8_t &value) {
    std::string s = trim_copy(text);
    if (s.empty()) {
        return false;
    }

    int base = 10;
    std::string digits = s;
    if (digits.size() > 2 && digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X')) {
        base = 16;
        digits = digits.substr(2);
    } else {
        bool looks_hex = false;
        for (char c : digits) {
            if ((c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')) {
                looks_hex = true;
                break;
            }
        }
        if (looks_hex || digits.size() == 2) {
            base = 16;
        }
    }

    char *end = nullptr;
    unsigned long parsed = std::strtoul(digits.c_str(), &end, base);
    if (end == nullptr || *end != '\0' || parsed > 0xffUL) {
        return false;
    }
    value = static_cast<uint8_t>(parsed);
    return true;
}

static bool parse_uint64_value(const std::string &text, SearchFormat format, uint64_t &value) {
    std::string s = trim_copy(text);
    if (s.empty()) {
        return false;
    }

    int base = format == SearchFormat::Hex ? 16 : 10;
    if (base == 16 && s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s = s.substr(2);
    }

    char *end = nullptr;
    unsigned long long parsed = std::strtoull(s.c_str(), &end, base);
    if (end == nullptr || *end != '\0') {
        return false;
    }
    value = static_cast<uint64_t>(parsed);
    return true;
}

static bool parse_memory_position(const std::string &text, size_t limit, size_t &value) {
    const std::string trimmed = trim_copy(text);
    if (trimmed.empty()) {
        return false;
    }

    uint64_t parsed = 0;
    const bool explicit_hex = trimmed.size() > 2 && trimmed[0] == '0' && (trimmed[1] == 'x' || trimmed[1] == 'X');
    bool has_hex_alpha = false;
    for (char c : trimmed) {
        if ((c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')) {
            has_hex_alpha = true;
            break;
        }
    }

    const SearchFormat format = (explicit_hex || has_hex_alpha) ? SearchFormat::Hex : SearchFormat::Decimal;
    if (!parse_uint64_value(trimmed, format, parsed)) {
        return false;
    }
    if (parsed >= static_cast<uint64_t>(limit)) {
        return false;
    }

    value = static_cast<size_t>(parsed);
    return true;
}

static bool parse_hex_byte_sequence(const std::string &text, size_t width, std::vector<uint8_t> &pattern) {
    const std::string trimmed = trim_copy(text);
    if (trimmed.empty()) {
        return false;
    }

    std::vector<uint8_t> parsed_bytes;

    if (trimmed.find_first_of(" \t\r\n,;:-") != std::string::npos) {
        std::istringstream input(trimmed);
        std::string token;
        while (input >> token) {
            uint8_t value = 0;
            if (!parse_byte_value(token, value)) {
                return false;
            }
            parsed_bytes.push_back(value);
        }
    } else {
        std::string digits = trimmed;
        if (digits.size() > 2 && digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X')) {
            digits = digits.substr(2);
        }
        if (digits.size() != width * 2) {
            return false;
        }
        for (size_t i = 0; i < digits.size(); i += 2) {
            uint8_t value = 0;
            if (!parse_byte_value(digits.substr(i, 2), value)) {
                return false;
            }
            parsed_bytes.push_back(value);
        }
    }

    if (parsed_bytes.size() != width) {
        return false;
    }

    pattern = std::move(parsed_bytes);
    return true;
}

static QString selection_summary_text(const std::vector<size_t> &positions) {
    if (positions.empty()) {
        return QStringLiteral("No bytes selected");
    }

    std::ostringstream ss;
    ss << positions.size() << " byte";
    if (positions.size() != 1) {
        ss << 's';
    }
    ss << " selected";

    const size_t first = positions.front();
    const size_t last = positions.back();
    ss << " | Range: 0x" << std::hex << std::uppercase;
    ss.width(8);
    ss.fill('0');
    ss << first;
    ss << "-0x";
    ss.width(8);
    ss.fill('0');
    ss << last;

    return QString::fromStdString(ss.str());
}

static QColor annotation_color_from_json(const QJsonValue &value) {
    if (!value.isString()) {
        return kDefaultAnnotationColor;
    }

    const QColor color(value.toString());
    return color.isValid() ? color : kDefaultAnnotationColor;
}

static bool annotation_position_from_json(const QJsonValue &value, size_t *position) {
    if (!position) {
        return false;
    }

    if (value.isDouble()) {
        const qint64 raw_value = static_cast<qint64>(value.toDouble());
        if (raw_value < 0) {
            return false;
        }
        *position = static_cast<size_t>(raw_value);
        return true;
    }

    if (value.isString()) {
        const QString text = value.toString().trimmed();
        bool ok = false;
        qulonglong parsed = 0;
        if (text.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive)) {
            parsed = text.mid(2).toULongLong(&ok, 16);
        } else {
            parsed = text.toULongLong(&ok, 10);
        }
        if (!ok) {
            return false;
        }
        *position = static_cast<size_t>(parsed);
        return true;
    }

    return false;
}

static QString annotation_position_to_json(size_t position) {
    return QStringLiteral("0x%1").arg(static_cast<qulonglong>(position), 0, 16);
}

static QString annotation_color_to_json(const QColor &color) {
    const QColor safe_color = color.isValid() ? color : kDefaultAnnotationColor;
    return safe_color.name(QColor::HexRgb);
}

static QString annotation_color_button_style(const QColor &color) {
    const QColor safe_color = color.isValid() ? color : kDefaultAnnotationColor;
    const QColor text_color = safe_color.lightness() < 128 ? QColor(Qt::white) : QColor(Qt::black);
    return QStringLiteral("QPushButton { background-color: %1; color: %2; }")
        .arg(safe_color.name(QColor::HexRgb), text_color.name(QColor::HexRgb));
}

static std::vector<QString> split_note_file_list(const QString &value) {
    std::vector<QString> paths;
    const QStringList raw_parts = value.split(QLatin1Char(':'), Qt::SkipEmptyParts);
    for (const QString &raw_part : raw_parts) {
        const QString trimmed = raw_part.trimmed();
        if (!trimmed.isEmpty()) {
            paths.push_back(trimmed);
        }
    }
    return paths;
}

static QString note_text_to_html(const QString &note) {
    QString html;
    html.reserve(note.size() * 2);

    int position = 0;
    while(position < note.size()) {
        const int marker_start = note.indexOf(QStringLiteral("[[jump:"), position);
        if(marker_start < 0) {
            html += note.mid(position).toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br/>"));
            break;
        }

        html += note.mid(position, marker_start - position).toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br/>"));
        const int separator = note.indexOf(QLatin1Char('|'), marker_start + 7);
        const int marker_end = note.indexOf(QStringLiteral("]]"), marker_start + 7);
        if(separator < 0 || marker_end < 0 || separator > marker_end) {
            html += note.mid(marker_start).toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br/>"));
            break;
        }

        const QString target = note.mid(marker_start + 7, separator - (marker_start + 7)).trimmed();
        const QString label = note.mid(separator + 1, marker_end - (separator + 1));
        html += QStringLiteral("<a href=\"jump:%1\">%2</a>")
            .arg(target.toHtmlEscaped(), label.toHtmlEscaped());
        position = marker_end + 2;
    }

    return QStringLiteral("<html><body style=\"font-family: monospace; white-space: pre-wrap;\">%1</body></html>").arg(html);
}

class AnnotationStore {
public:
    struct ResolvedAnnotation {
        std::vector<size_t> positions;
        std::string note;
        QColor color = kDefaultAnnotationColor;

        bool isValid() const {
            return !positions.empty();
        }
    };

    bool hasFilePath() const {
        return !file_path_.isEmpty();
    }

    QString filePath() const {
        return file_path_;
    }

    bool selectFile(const QString &path, QString *error_message = nullptr) {
        if (path.isEmpty()) {
            return false;
        }
        const double start = mem_viewer_now_seconds();

        file_path_ = path;
        annotations_.clear();
        position_to_annotation_indices_.clear();

        QFile file(file_path_);
        if (!file.exists()) {
            return save(error_message);
        }

        if (!file.open(QIODevice::ReadOnly)) {
            if (error_message) {
                *error_message = QStringLiteral("Failed to open %1: %2").arg(file_path_, file.errorString());
            }
            return false;
        }

        const QByteArray json_data = file.readAll();
        mem_viewer_debug_log("notes read path=%s bytes=%lld in %.3f s",
            path.toLocal8Bit().constData(),
            static_cast<long long>(json_data.size()),
            mem_viewer_now_seconds() - start);
        QJsonParseError parse_error{};
        const double parse_start = mem_viewer_now_seconds();
        const QJsonDocument document = QJsonDocument::fromJson(json_data, &parse_error);
        if (parse_error.error != QJsonParseError::NoError || !document.isObject()) {
            if (error_message) {
                *error_message = QStringLiteral("Invalid JSON in %1: %2").arg(file_path_, parse_error.errorString());
            }
            annotations_.clear();
            return false;
        }

        const QJsonArray annotations = document.object().value(QStringLiteral("annotations")).toArray();
        annotations_.reserve(static_cast<size_t>(annotations.size()));
        for (const QJsonValue &entry_value : annotations) {
            if (!entry_value.isObject()) {
                continue;
            }
            const QJsonObject entry_object = entry_value.toObject();
            const QJsonArray positions_array = entry_object.value(QStringLiteral("positions")).toArray();
            std::vector<size_t> unique_positions;
            unique_positions.reserve(static_cast<size_t>(positions_array.size()));
            for (const QJsonValue &position_value : positions_array) {
                size_t position = 0;
                if (!annotation_position_from_json(position_value, &position)) {
                    continue;
                }
                unique_positions.push_back(position);
            }
            if (unique_positions.empty()) {
                continue;
            }
            std::sort(unique_positions.begin(), unique_positions.end());
            unique_positions.erase(std::unique(unique_positions.begin(), unique_positions.end()), unique_positions.end());

            AnnotationEntry entry;
            entry.positions = std::move(unique_positions);
            entry.note = entry_object.value(QStringLiteral("note")).toString().toStdString();
            entry.color = annotation_color_from_json(entry_object.value(QStringLiteral("color")));
            annotations_.push_back(std::move(entry));
        }

        annotation_points_dirty_ = true;
        annotation_lookup_dirty_ = true;
        rebuildAnnotationPointsIfNeeded();
        rebuildLookupIfNeeded();
        mem_viewer_debug_log("notes parsed path=%s annotations=%zu parse=%.3f s total=%.3f s",
            path.toLocal8Bit().constData(),
            annotations_.size(),
            mem_viewer_now_seconds() - parse_start,
            mem_viewer_now_seconds() - start);

        return true;
    }

    ResolvedAnnotation resolveForSelection(const std::vector<size_t> &positions) const {
        if (positions.empty()) {
            return {};
        }
        const double start = mem_viewer_now_seconds();
        rebuildLookupIfNeeded();

        const AnnotationEntry *best_match = nullptr;
        auto consider_entry = [&](const AnnotationEntry &entry) {
            if (entry.positions == positions) {
                best_match = &entry;
                return true;
            }
            if (!containsAllPositions(entry.positions, positions)) {
                return false;
            }
            if (best_match == nullptr || entry.positions.size() < best_match->positions.size()) {
                best_match = &entry;
            }
            return false;
        };

        if (const auto candidates_it = position_to_annotation_indices_.find(positions.front());
            candidates_it != position_to_annotation_indices_.end()) {
            for (size_t entry_index : candidates_it->second) {
                if (entry_index >= annotations_.size()) {
                    continue;
                }
                if (consider_entry(annotations_[entry_index])) {
                    mem_viewer_debug_log("resolveForSelection positions=%zu candidates=%zu fast=1 elapsed=%.6f s",
                        positions.size(),
                        candidates_it->second.size(),
                        mem_viewer_now_seconds() - start);
                    return {best_match->positions, best_match->note, best_match->color};
                }
            }
        }

        for (const AnnotationEntry &entry : annotations_) {
            if (consider_entry(entry)) {
                mem_viewer_debug_log("resolveForSelection positions=%zu candidates=%zu fast=0 elapsed=%.6f s",
                    positions.size(),
                    annotations_.size(),
                    mem_viewer_now_seconds() - start);
                return {best_match->positions, best_match->note, best_match->color};
            }
        }

        if (best_match == nullptr) {
            mem_viewer_debug_log("resolveForSelection positions=%zu candidates=0 elapsed=%.6f s",
                positions.size(),
                mem_viewer_now_seconds() - start);
            return {};
        }
        mem_viewer_debug_log("resolveForSelection positions=%zu elapsed=%.6f s",
            positions.size(),
            mem_viewer_now_seconds() - start);
        return {best_match->positions, best_match->note, best_match->color};
    }

    bool setAnnotation(const std::vector<size_t> &positions, const std::string &note, const QColor &color, QString *error_message = nullptr) {
        if (positions.empty()) {
            return true;
        }

        const std::vector<size_t> normalized_positions = normalizePositions(positions);
        removePositionsFromAnnotations(normalized_positions);

        if (!trim_copy(note).empty()) {
            AnnotationEntry entry;
            entry.positions = normalized_positions;
            entry.note = note;
            entry.color = color.isValid() ? color : kDefaultAnnotationColor;
            annotations_.push_back(std::move(entry));
        }
        annotation_points_dirty_ = true;
        annotation_lookup_dirty_ = true;
        return save(error_message);
    }

    bool clearAnnotationPositions(const std::vector<size_t> &positions, QString *error_message = nullptr) {
        if (positions.empty()) {
            return true;
        }

        removePositionsFromAnnotations(normalizePositions(positions));
        annotation_points_dirty_ = true;
        annotation_lookup_dirty_ = true;
        return save(error_message);
    }

    const std::vector<AnnotationColorPoint> &annotatedPositions() const {
        rebuildAnnotationPointsIfNeeded();
        return annotation_points_;
    }

    std::vector<ResolvedAnnotation> searchNotes(const QString &query) const {
        std::vector<ResolvedAnnotation> matches;
        if(query.trimmed().isEmpty()) {
            return matches;
        }

        const QString needle = query.toCaseFolded();
        for(const AnnotationEntry &entry : annotations_) {
            if(QString::fromStdString(entry.note).toCaseFolded().contains(needle)) {
                matches.push_back({entry.positions, entry.note, entry.color});
            }
        }
        return matches;
    }

private:
    static std::vector<size_t> normalizePositions(const std::vector<size_t> &positions) {
        std::vector<size_t> unique_positions = positions;
        std::sort(unique_positions.begin(), unique_positions.end());
        unique_positions.erase(std::unique(unique_positions.begin(), unique_positions.end()), unique_positions.end());
        return unique_positions;
    }

    static bool containsAllPositions(const std::vector<size_t> &haystack, const std::vector<size_t> &needle) {
        if (needle.empty() || haystack.empty()) {
            return false;
        }

        size_t haystack_index = 0;
        for (size_t position : needle) {
            while (haystack_index < haystack.size() && haystack[haystack_index] < position) {
                ++haystack_index;
            }
            if (haystack_index >= haystack.size() || haystack[haystack_index] != position) {
                return false;
            }
        }
        return true;
    }

    void removePositionsFromAnnotations(const std::vector<size_t> &positions_to_remove) {
        std::vector<AnnotationEntry> updated_annotations;
        updated_annotations.reserve(annotations_.size() + 1);

        for (const AnnotationEntry &entry : annotations_) {
            std::vector<size_t> remaining_positions;
            remaining_positions.reserve(entry.positions.size());

            size_t remove_index = 0;
            for (size_t position : entry.positions) {
                while (remove_index < positions_to_remove.size() && positions_to_remove[remove_index] < position) {
                    ++remove_index;
                }
                if (remove_index < positions_to_remove.size() && positions_to_remove[remove_index] == position) {
                    continue;
                }
                remaining_positions.push_back(position);
            }

            if (remaining_positions.empty()) {
                continue;
            }

            AnnotationEntry updated_entry;
            updated_entry.positions = std::move(remaining_positions);
            updated_entry.note = entry.note;
            updated_entry.color = entry.color;
            updated_annotations.push_back(std::move(updated_entry));
        }

        annotations_ = std::move(updated_annotations);
        annotation_points_dirty_ = true;
        annotation_lookup_dirty_ = true;
    }

    void rebuildAnnotationPointsIfNeeded() const {
        if (!annotation_points_dirty_) {
            return;
        }

        size_t total_positions = 0;
        for (const AnnotationEntry &entry : annotations_) {
            total_positions += entry.positions.size();
        }

        std::vector<AnnotationColorPoint> points;
        points.reserve(total_positions);
        for (const AnnotationEntry &entry : annotations_) {
            const QRgb color = entry.color.isValid() ? entry.color.rgba() : kDefaultAnnotationColor.rgba();
            for (size_t position : entry.positions) {
                points.push_back({position, color});
            }
        }

        std::sort(points.begin(), points.end(), [](const AnnotationColorPoint &lhs, const AnnotationColorPoint &rhs) {
            if (lhs.position != rhs.position) {
                return lhs.position < rhs.position;
            }
            return lhs.color < rhs.color;
        });

        size_t write_index = 0;
        for (size_t read_index = 0; read_index < points.size(); ++read_index) {
            while (read_index + 1 < points.size() && points[read_index + 1].position == points[read_index].position) {
                ++read_index;
            }
            points[write_index++] = points[read_index];
        }
        points.resize(write_index);

        annotation_points_ = std::move(points);
        annotation_points_dirty_ = false;
    }

    void rebuildLookupIfNeeded() const {
        if (!annotation_lookup_dirty_) {
            return;
        }

        const double start = mem_viewer_now_seconds();
        std::unordered_map<size_t, std::vector<size_t>> lookup;
        lookup.reserve(annotation_points_.empty() ? annotations_.size() : annotation_points_.size());
        for (size_t entry_index = 0; entry_index < annotations_.size(); ++entry_index) {
            const AnnotationEntry &entry = annotations_[entry_index];
            for (size_t position : entry.positions) {
                lookup[position].push_back(entry_index);
            }
        }

        for (auto &[position, indices] : lookup) {
            std::sort(indices.begin(), indices.end(), [this](size_t lhs, size_t rhs) {
                const size_t lhs_size = lhs < annotations_.size() ? annotations_[lhs].positions.size() : std::numeric_limits<size_t>::max();
                const size_t rhs_size = rhs < annotations_.size() ? annotations_[rhs].positions.size() : std::numeric_limits<size_t>::max();
                if (lhs_size != rhs_size) {
                    return lhs_size < rhs_size;
                }
                return lhs < rhs;
            });
        }

        position_to_annotation_indices_ = std::move(lookup);
        annotation_lookup_dirty_ = false;
        mem_viewer_debug_log("annotation lookup rebuilt entries=%zu positions=%zu in %.3f s",
            annotations_.size(),
            position_to_annotation_indices_.size(),
            mem_viewer_now_seconds() - start);
    }

    bool save(QString *error_message) const {
        if (file_path_.isEmpty()) {
            return true;
        }

        QJsonArray annotation_array;
        for (const AnnotationEntry &entry : annotations_) {
            QJsonObject object;
            QJsonArray positions;
            for (size_t position : entry.positions) {
                positions.append(annotation_position_to_json(position));
            }
            object.insert(QStringLiteral("positions"), positions);
            object.insert(QStringLiteral("note"), QString::fromStdString(entry.note));
            object.insert(QStringLiteral("color"), annotation_color_to_json(entry.color));
            annotation_array.append(object);
        }

        QJsonObject root;
        root.insert(QStringLiteral("annotations"), annotation_array);
        const QJsonDocument document(root);

        QFile file(file_path_);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            if (error_message) {
                *error_message = QStringLiteral("Failed to write %1: %2").arg(file_path_, file.errorString());
            }
            return false;
        }
        file.write(document.toJson(QJsonDocument::Indented));
        return true;
    }

    QString file_path_;
    std::vector<AnnotationEntry> annotations_;
    mutable std::vector<AnnotationColorPoint> annotation_points_;
    mutable bool annotation_points_dirty_ = true;
    mutable std::unordered_map<size_t, std::vector<size_t>> position_to_annotation_indices_;
    mutable bool annotation_lookup_dirty_ = true;
};

class NoteScrollBar : public QWidget {
public:
    explicit NoteScrollBar(QWidget *parent = nullptr)
        : QWidget(parent) {
        setFixedWidth(10);
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    }

    void setMemorySize(size_t memory_size) {
        memory_size_ = memory_size;
        row_count_ = memory_size_ == 0 ? 0 : ((memory_size_ + kBytesPerRow - 1) / kBytesPerRow);
        update();
    }

    void setAnnotatedPositions(const std::vector<AnnotationColorPoint> &positions) {
        annotated_positions_ = positions;
        update();
    }

    std::function<void(size_t)> onAnnotatedRowActivated;

protected:
    void mousePressEvent(QMouseEvent *event) override {
        if (event != nullptr && event->button() == Qt::LeftButton) {
            const std::optional<size_t> row = annotatedRowForClick(event->position().toPoint());
            if (row.has_value()) {
                if (onAnnotatedRowActivated) {
                    onAnnotatedRowActivated(*row);
                }
                event->accept();
                return;
            }
        }

        QWidget::mousePressEvent(event);
    }

    void paintEvent(QPaintEvent *event) override {
        QWidget::paintEvent(event);

        QPainter background_painter(this);
        background_painter.fillRect(rect(), palette().window().color().darker(108));

        if (row_count_ == 0 || annotated_positions_.empty()) {
            return;
        }

        const QRect groove = rect().adjusted(1, 2, -1, -2);
        if (!groove.isValid() || groove.height() <= 0) {
            return;
        }

        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, false);
        painter.setPen(Qt::NoPen);
        painter.setBrush(kDefaultAnnotationColor);

        int previous_y = std::numeric_limits<int>::min();
        for (const AnnotationColorPoint &point : annotated_positions_) {
            const size_t row = point.position / kBytesPerRow;
            const double ratio = row_count_ <= 1
                ? 0.0
                : static_cast<double>(row) / static_cast<double>(row_count_ - 1);
            int y = groove.top() + static_cast<int>(std::floor(ratio * static_cast<double>(groove.height() - 1)));
            y = std::clamp(y, groove.top(), groove.bottom());
            if (y == previous_y) {
                continue;
            }
            previous_y = y;
            QColor marker_color = QColor::fromRgba(point.color);
            marker_color.setAlpha(220);
            painter.setBrush(marker_color);
            painter.drawRect(QRect(groove.left() + 1, y, std::max(2, groove.width() - 2), 2));
        }
    }

private:
    std::optional<size_t> annotatedRowForClick(const QPoint &point) const {
        if (row_count_ == 0 || annotated_positions_.empty()) {
            return std::nullopt;
        }

        const QRect groove = rect().adjusted(1, 2, -1, -2);
        if (!groove.isValid() || groove.height() <= 0 || !groove.contains(point)) {
            return std::nullopt;
        }

        const int click_y = std::clamp(point.y(), groove.top(), groove.bottom());
        int previous_y = std::numeric_limits<int>::min();
        size_t best_row = 0;
        int best_distance = std::numeric_limits<int>::max();
        for (const AnnotationColorPoint &annotation : annotated_positions_) {
            const size_t row = annotation.position / kBytesPerRow;
            const int marker_y = markerYForRow(groove, row);
            if (marker_y == previous_y) {
                continue;
            }
            previous_y = marker_y;
            const int distance = std::min(std::abs(click_y - marker_y), std::abs(click_y - (marker_y + 1)));
            if (distance < best_distance) {
                best_distance = distance;
                best_row = row;
            }
        }

        return best_distance == std::numeric_limits<int>::max() ? std::nullopt : std::optional<size_t>(best_row);
    }

    int markerYForRow(const QRect &groove, size_t row) const {
        const double ratio = row_count_ <= 1
            ? 0.0
            : static_cast<double>(row) / static_cast<double>(row_count_ - 1);
        int y = groove.top() + static_cast<int>(std::floor(ratio * static_cast<double>(groove.height() - 1)));
        return std::clamp(y, groove.top(), groove.bottom());
    }

    size_t memory_size_ = 0;
    size_t row_count_ = 0;
    std::vector<AnnotationColorPoint> annotated_positions_;
};

class RemoteMemory {
public:
    RemoteMemory(pid_t pid, uintptr_t address, size_t size)
        : pid_(pid), address_(address), size_(size) {}

    size_t size() const {
        return size_;
    }

    bool read(size_t offset, void *buffer, size_t length) const {
        if (offset >= size_) {
            return false;
        }
        const size_t bounded = std::min(length, size_ - offset);
        struct iovec local = {buffer, bounded};
        struct iovec remote = {reinterpret_cast<void *>(address_ + offset), bounded};
        const ssize_t result = process_vm_readv(pid_, &local, 1, &remote, 1, 0);
        if (result != static_cast<ssize_t>(bounded) && mem_viewer_debug_enabled()) {
            mem_viewer_debug_log("process_vm_readv failed pid=%ld offset=%zu length=%zu result=%zd errno=%d (%s)",
                static_cast<long>(pid_), offset, bounded, result, errno, std::strerror(errno));
        }
        return result == static_cast<ssize_t>(bounded);
    }

    bool write(size_t offset, const void *buffer, size_t length) const {
        if (offset >= size_) {
            return false;
        }
        const size_t bounded = std::min(length, size_ - offset);
        struct iovec local = {const_cast<void *>(buffer), bounded};
        struct iovec remote = {reinterpret_cast<void *>(address_ + offset), bounded};
        const ssize_t result = process_vm_writev(pid_, &local, 1, &remote, 1, 0);
        if (result != static_cast<ssize_t>(bounded) && mem_viewer_debug_enabled()) {
            mem_viewer_debug_log("process_vm_writev failed pid=%ld offset=%zu length=%zu result=%zd errno=%d (%s)",
                static_cast<long>(pid_), offset, bounded, result, errno, std::strerror(errno));
        }
        return result == static_cast<ssize_t>(bounded);
    }

    bool read_byte(size_t offset, uint8_t &value) const {
        return read(offset, &value, 1);
    }

    bool write_byte(size_t offset, uint8_t value) const {
        return write(offset, &value, 1);
    }

private:
    pid_t pid_;
    uintptr_t address_;
    size_t size_;
};

class LocalMemory {
public:
    LocalMemory(void *memory, size_t size)
        : memory_(static_cast<uint8_t *>(memory)), size_(size) {}

    size_t size() const {
        return size_;
    }

    bool read(size_t offset, void *buffer, size_t length) const {
        if (offset >= size_) {
            return false;
        }
        const size_t bounded = std::min(length, size_ - offset);
        std::memcpy(buffer, memory_ + offset, bounded);
        return true;
    }

    bool write(size_t offset, const void *buffer, size_t length) const {
        if (offset >= size_) {
            return false;
        }
        const size_t bounded = std::min(length, size_ - offset);
        std::memcpy(memory_ + offset, buffer, bounded);
        return true;
    }

    bool read_byte(size_t offset, uint8_t &value) const {
        return read(offset, &value, 1);
    }

    bool write_byte(size_t offset, uint8_t value) const {
        return write(offset, &value, 1);
    }

private:
    uint8_t *memory_;
    size_t size_;
};

class MemViewerWidget : public QWidget {
public:
    MemViewerWidget(
        size_t memory_size,
        std::function<bool(size_t, void *, size_t)> read_memory,
        std::function<bool(size_t, const void *, size_t)> write_memory,
        std::atomic<bool> &open_flag,
        QWidget *parent = nullptr)
        : QWidget(parent),
          memory_size_(memory_size),
          read_memory_(std::move(read_memory)),
          write_memory_(std::move(write_memory)),
          open_flag_(open_flag),
          rows_(memory_size_ == 0 ? 0 : ((memory_size_ + kBytesPerRow - 1) / kBytesPerRow)),
          last_seen_(memory_size_, 0),
          changed_at_(memory_size_, -1.0),
          match_mask_(memory_size_, 0),
          selection_mask_(memory_size_, 0),
          font_("Monospace", 11) {
        
        font_.setStyleHint(QFont::Monospace);
        font_.setFixedPitch(true);
        QFontMetrics fm(font_);

        char_width_ = fm.horizontalAdvance('W'); 
        row_height_ = static_cast<double>(fm.lineSpacing()) + 2.0;
        baseline_y_ = static_cast<double>(fm.ascent()) + 1.0;
        
        address_x_ = kAddressX;
        hex_cell_width_ = char_width_; 
        ascii_cell_width_ = char_width_;
        
        hex_start_x_ = address_x_ + kAddressChars * char_width_ + kGapAddressToHex;
        ascii_start_x_ = hex_start_x_ + (kBytesPerRow * 3 - 1) * hex_cell_width_ + kGapHexToAscii;
        content_width_ = ascii_start_x_ + kBytesPerRow * ascii_cell_width_ + 4.0;
        
        hex_highlight_width_ = hex_cell_width_ * 2.2;
        ascii_highlight_width_ = ascii_cell_width_ * 1.2;

        setFocusPolicy(Qt::StrongFocus);
        setMouseTracking(true);
        setMinimumWidth(static_cast<int>(content_width_));
        setMinimumHeight(static_cast<int>(rows_ * row_height_));
        
        timer_ = new QTimer(this);
        connect(timer_, &QTimer::timeout, this, [this]() { onTimer(); });
        if (!mem_viewer_auto_refresh_forced_off()) {
            timer_->start(kRefreshMs);
        }
        
        refreshVisibleBytes();
    }

    ~MemViewerWidget() override {
        open_flag_.store(false, std::memory_order_relaxed);
    }

    void setAutoRefresh(bool enabled) {
        if (enabled) {
            timer_->start(kRefreshMs);
        } else {
            timer_->stop();
        }
    }

    void refreshVisibleBytes() {
        const double start = mem_viewer_now_seconds();
        if (memory_size_ == 0) {
            update();
            return;
        }

        QScrollBar *vscroll = verticalScrollBar();
        if (!vscroll) return;
        
        const int scroll_y = vscroll->value();
        const int visible_height = viewport()->height();
        const size_t first_row = static_cast<size_t>(std::max(0, static_cast<int>(std::floor(scroll_y / row_height_))));
        const size_t visible_rows = static_cast<size_t>(std::ceil(visible_height / row_height_)) + 2;
        const size_t last_row = std::min(rows_, first_row + visible_rows);
        const size_t begin = std::min(memory_size_, first_row * kBytesPerRow);
        const size_t end = std::min(memory_size_, last_row * kBytesPerRow);
        const double now = mem_viewer_now_seconds();

        visible_begin_ = begin;
        visible_end_ = end;
        visible_cache_.resize(end > begin ? (end - begin) : 0);

        if (end > begin) {
            if (!read_memory_(begin, visible_cache_.data(), visible_cache_.size())) {
                std::fill(visible_cache_.begin(), visible_cache_.end(), 0);
            }
        }

        for (size_t i = 0; i < visible_cache_.size(); ++i) {
            const size_t index = begin + i;
            const uint8_t current = visible_cache_[i];
            if (current != last_seen_[index]) {
                last_seen_[index] = current;
                changed_at_[index] = now;
            }
        }

        for (size_t index : selected_indices_) {
            if (index >= memory_size_) {
                continue;
            }
            uint8_t value_byte = 0;
            if (read_memory_(index, &value_byte, 1)) {
                last_seen_[index] = value_byte;
            }
        }

        update();
        if (!logged_first_refresh_) {
            logged_first_refresh_ = true;
            mem_viewer_debug_log("refreshVisibleBytes first begin=%zu end=%zu cache=%zu elapsed=%.6f s",
                begin,
                end,
                visible_cache_.size(),
                mem_viewer_now_seconds() - start);
        }
    }

    void rebuildSearch(const std::string &search_text, SearchFormat format, EndianMode endian, size_t width) {
        std::fill(match_mask_.begin(), match_mask_.end(), 0);
        matches_.clear();
        active_match_index_ = 0;

        if (width == 0 || width > 8 || width > memory_size_) {
            update();
            if (onSearchStatusUpdated) onSearchStatusUpdated();
            return;
        }

        std::vector<uint8_t> pattern(width);
        if (format == SearchFormat::Hex && parse_hex_byte_sequence(search_text, width, pattern)) {
            // Explicit byte sequences are already in the typed search order.
        } else {
            uint64_t value = 0;
            if (!parse_uint64_value(search_text, format, value)) {
                update();
                if (onSearchStatusUpdated) onSearchStatusUpdated();
                return;
            }

            if (endian == EndianMode::Little) {
                for (size_t i = 0; i < width; ++i) {
                    pattern[i] = static_cast<uint8_t>((value >> (8 * i)) & 0xffU);
                }
            } else {
                for (size_t i = 0; i < width; ++i) {
                    pattern[width - 1 - i] = static_cast<uint8_t>((value >> (8 * i)) & 0xffU);
                }
            }
        }

        const size_t chunk_size = std::max(kSearchChunkBytes, width);
        std::vector<uint8_t> chunk(chunk_size + width);
        size_t offset = 0;

        while (offset < memory_size_) {
            const size_t span = std::min(chunk_size, memory_size_ - offset);
            if (!read_memory_(offset, chunk.data(), span)) {
                break;
            }

            for (size_t i = 0; i + width <= span; ++i) {
                if (std::memcmp(chunk.data() + i, pattern.data(), width) != 0) {
                    continue;
                }
                const size_t match_index = offset + i;
                matches_.push_back(match_index);
                for (size_t j = 0; j < width; ++j) {
                    match_mask_[match_index + j] = 1;
                }
            }

            if (offset + span >= memory_size_) {
                break;
            }
            offset += span >= width ? (span - width + 1) : span;
        }

        if (!matches_.empty()) {
            setSingleSelection(matches_.front());
            scroll_to_index(matches_.front());
        }
        if (onSearchStatusUpdated) onSearchStatusUpdated();
        update();
    }

    void navigateMatch(int direction) {
        if (matches_.empty()) {
            return;
        }
        const size_t count = matches_.size();
        if (direction < 0) {
            active_match_index_ = (active_match_index_ + count - 1) % count;
        } else {
            active_match_index_ = (active_match_index_ + 1) % count;
        }
        setSingleSelection(matches_[active_match_index_]);
        scroll_to_index(matches_[active_match_index_]);
        refreshVisibleBytes();
    }

    void setSelectedIndex(size_t index) {
        if (index >= memory_size_) {
            return;
        }
        setSingleSelection(index);
        uint8_t value = last_seen_[index];
        if (read_memory_(index, &value, 1)) {
            last_seen_[index] = value;
        }
        update();
    }

    bool jumpToIndex(size_t index) {
        if (index >= memory_size_) {
            return false;
        }
        setSelectedIndex(index);
        scroll_to_index(index);
        refreshVisibleBytes();
        return true;
    }

    size_t getSelectedIndex() const {
        return selected_indices_.size() == 1 ? selected_indices_.front() : std::numeric_limits<size_t>::max();
    }

    uint8_t getSelectedValue() const {
        const size_t selected_index = getSelectedIndex();
        if (selected_index >= memory_size_) {
            return 0;
        }
        return last_seen_[selected_index];
    }

    const std::vector<size_t> &getSelectedIndices() const {
        return selected_indices_;
    }

    size_t getMatchCount() const {
        return matches_.size();
    }

    size_t getMemorySize() const {
        return memory_size_;
    }

    void applyEdit(uint8_t value) {
        const size_t selected_index = getSelectedIndex();
        if (selected_index >= memory_size_) {
            return;
        }
        if (!write_memory_(selected_index, &value, 1)) {
            return;
        }
        last_seen_[selected_index] = value;
        changed_at_[selected_index] = mem_viewer_now_seconds();
        refreshVisibleBytes();
    }

    std::string selectedContentText() const {
        if (selected_indices_.empty()) {
            return {};
        }

        std::vector<uint8_t> values(selected_indices_.size(), 0);
        for (size_t i = 0; i < selected_indices_.size(); ++i) {
            const size_t index = selected_indices_[i];
            uint8_t value = last_seen_[index];
            if (read_memory_(index, &value, 1)) {
                values[i] = value;
            } else {
                values[i] = value;
            }
        }

        std::ostringstream ss;
        size_t i = 0;
        while (i < selected_indices_.size()) {
            const size_t row = selected_indices_[i] / kBytesPerRow;
            const size_t row_base = row * kBytesPerRow;
            if (i > 0) {
                ss << '\n';
            }

            ss << std::hex << std::uppercase;
            ss.width(8);
            ss.fill('0');
            ss << row_base << ": ";

            std::string ascii;
            bool first_in_row = true;
            while (i < selected_indices_.size() && (selected_indices_[i] / kBytesPerRow) == row) {
                if (!first_in_row) {
                    ss << ' ';
                }
                first_in_row = false;
                ss.width(2);
                ss.fill('0');
                ss << static_cast<unsigned>(values[i]);
                ascii.push_back(printable(values[i]));
                ++i;
            }
            ss << "  |" << ascii << '|';
        }

        return ss.str();
    }

    void setAnnotatedPositions(const std::vector<AnnotationColorPoint> &positions) {
        annotation_points_ = positions;
        update();
    }

    std::function<void(const std::vector<size_t> &)> onSelectionChanged;
    std::function<void()> onSearchStatusUpdated;

protected:
    void paintEvent(QPaintEvent *event) override {
        const double start = mem_viewer_now_seconds();
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        
        const QRect dirty_rect = event != nullptr ? event->rect() : rect();
        const int dirty_top = std::max(0, dirty_rect.top());
        const int dirty_bottom = std::min(height(), dirty_rect.bottom() + 1);
        const size_t first_row = static_cast<size_t>(std::max(0, static_cast<int>(std::floor(static_cast<double>(dirty_top) / row_height_))));
        const size_t visible_rows = static_cast<size_t>(std::ceil(static_cast<double>(std::max(0, dirty_bottom - dirty_top)) / row_height_)) + 2;
        const size_t last_row = std::min(rows_, first_row + visible_rows);
        const double now = mem_viewer_now_seconds();

        painter.fillRect(rect(), QColor(0x14, 0x16, 0x1A));

        painter.setFont(font_);
        auto annotation_it = std::lower_bound(
            annotation_points_.begin(),
            annotation_points_.end(),
            first_row * kBytesPerRow,
            [](const AnnotationColorPoint &point, size_t position) {
                return point.position < position;
            });

        for (size_t row = first_row; row < last_row; ++row) {
            const double y = static_cast<double>(row * row_height_);
            const size_t row_base = row * kBytesPerRow;

            if ((row % 2) == 0) {
                painter.fillRect(QRectF(0, y, width(), row_height_), QColor(0xFF, 0xFF, 0xFF, 6));
            }

            char addr[32];
            std::snprintf(addr, sizeof(addr), "%08zx", row_base);
            drawText(painter, address_x_, y + baseline_y_, QColor(0xBD, 0xC7, 0xD0), addr);

            for (size_t col = 0; col < kBytesPerRow; ++col) {
                const size_t index = row_base + col;
                if (index >= memory_size_) {
                    break;
                }

                while (annotation_it != annotation_points_.end() && annotation_it->position < index) {
                    ++annotation_it;
                }
                const bool annotated = annotation_it != annotation_points_.end() && annotation_it->position == index;
                const QColor annotation_color = annotated ? QColor::fromRgba(annotation_it->color) : kDefaultAnnotationColor;

                const double cell_x = hex_start_x_ + static_cast<double>(col * 3) * hex_cell_width_;
                const double ascii_x = ascii_start_x_ + static_cast<double>(col) * ascii_cell_width_;
                const bool selected = selection_mask_[index] != 0;
                const bool matched = match_mask_[index] != 0;
                const double age = changed_at_[index] < 0.0 ? kFadeSeconds : (now - changed_at_[index]);
                const double fade = std::clamp(1.0 - (age / kFadeSeconds), 0.0, 1.0);
                const uint8_t value = byteForIndex(index);

                if (fade > 0.0 || selected || matched) {
                    double r = 0.14;
                    double g = 0.14;
                    double b = 0.16;
                    int a = 0;

                    if (fade > 0.0) {
                        r = 0.96;
                        g = 0.38;
                        b = 0.14;
                        a = static_cast<int>(255 * (0.16 + 0.42 * fade));
                    }
                    if (matched) {
                        r = 0.96;
                        g = 0.79;
                        b = 0.18;
                        a = std::max(a, static_cast<int>(255 * 0.42));
                    }
                    if (selected) {
                        r = 0.22;
                        g = 0.64;
                        b = 1.0;
                        a = std::max(a, static_cast<int>(255 * 0.35));
                    }

                    painter.fillRect(QRectF(cell_x - 2.0, y + 1.5, hex_highlight_width_, row_height_ - 3.0),
                                     QColor(static_cast<int>(r * 255), static_cast<int>(g * 255), static_cast<int>(b * 255), a));
                    painter.fillRect(QRectF(ascii_x - 1.0, y + 1.5, ascii_highlight_width_, row_height_ - 3.0),
                                     QColor(static_cast<int>(r * 255), static_cast<int>(g * 255), static_cast<int>(b * 255), a));
                }

                char hex[4];
                std::snprintf(hex, sizeof(hex), "%02X", value);
                QColor hex_color(0xEE, 0xEE, 0xEF);
                QColor ascii_color(0xCD, 0xDB, 0xE2);
                if (annotated) {
                    hex_color = annotation_color;
                    ascii_color = annotation_color;
                }
                if (matched) {
                    hex_color = QColor(0xFF, 0xF4, 0xB0);
                    ascii_color = QColor(0xFF, 0xF4, 0xB0);
                }
                if (selected) {
                    hex_color = QColor(0xFF, 0xFF, 0xFF);
                    ascii_color = QColor(0xFF, 0xFF, 0xFF);
                }
                drawText(
                    painter,
                    cell_x,
                    y + baseline_y_,
                    hex_color,
                    hex);

                char ascii[2] = { printable(value), '\0' };
                drawText(
                    painter,
                    ascii_x,
                    y + baseline_y_,
                    ascii_color,
                    ascii);
            }
        }
        if (!logged_first_paint_) {
            logged_first_paint_ = true;
            mem_viewer_debug_log("paintEvent first rows=%zu elapsed=%.6f s",
                last_row > first_row ? (last_row - first_row) : 0,
                mem_viewer_now_seconds() - start);
        }
    }

    void mousePressEvent(QMouseEvent *event) override {
        if (event->button() != Qt::LeftButton) {
            return;
        }

        const std::optional<size_t> index = indexForPosition(event->position());
        if (!index.has_value()) {
            return;
        }

        const Qt::KeyboardModifiers modifiers = event->modifiers();
        if ((modifiers & Qt::ShiftModifier) != 0 && selection_anchor_ < memory_size_) {
            selection_drag_anchor_ = selection_anchor_;
            drag_selecting_ = true;
            setRangeSelection(selection_anchor_, *index);
        } else {
            selection_drag_anchor_ = *index;
            drag_selecting_ = true;
            setSelectedIndex(*index);
        }

        event->accept();
    }

    void mouseMoveEvent(QMouseEvent *event) override {
        if (!drag_selecting_ || (event->buttons() & Qt::LeftButton) == 0) {
            return;
        }

        const std::optional<size_t> index = indexForPosition(event->position());
        if (!index.has_value()) {
            return;
        }

        setRangeSelection(selection_drag_anchor_, *index);
        event->accept();
    }

    void mouseReleaseEvent(QMouseEvent *event) override {
        if (event->button() == Qt::LeftButton) {
            drag_selecting_ = false;
            event->accept();
            return;
        }
        QWidget::mouseReleaseEvent(event);
    }

    void wheelEvent(QWheelEvent *event) override {
        QScrollBar *vscroll = verticalScrollBar();
        if (vscroll) {
            const int delta = event->angleDelta().y();
            if (delta != 0) {
                const int numDegrees = delta / 8;
                const int numSteps = numDegrees / 15;
                for (int i = 0; i < std::abs(numSteps); ++i) {
                    if (numSteps > 0)
                        vscroll->triggerAction(QScrollBar::SliderSingleStepSub);
                    else
                        vscroll->triggerAction(QScrollBar::SliderSingleStepAdd);
                }
            }
            event->accept();
        }
    }

    void resizeEvent(QResizeEvent *event) override {
        QWidget::resizeEvent(event);
        updateGeometry();
    }

private:
    void onTimer() {
        refreshVisibleBytes();
    }

private:
    uint8_t byteForIndex(size_t index) const {
        if (index >= visible_begin_ && index < visible_end_) {
            return visible_cache_[index - visible_begin_];
        }
        return last_seen_[index];
    }

    void drawText(QPainter &painter, double x, double y, const QColor &color, const char *text) {
        painter.setPen(color);
        painter.drawText(QPointF(x, y), text);
    }

    static char printable(uint8_t value) {
        return std::isprint(static_cast<unsigned char>(value)) != 0 ? static_cast<char>(value) : '.';
    }

    void scroll_to_index(size_t index) {
        QScrollBar *vscroll = verticalScrollBar();
        if (!vscroll) return;
        
        const double row_top = static_cast<double>((index / kBytesPerRow) * row_height_);
        const double row_bottom = row_top + row_height_;
        const double view_top = static_cast<double>(vscroll->value());
        const double view_bottom = view_top + vscroll->pageStep();
        if (row_top < view_top) {
            vscroll->setValue(static_cast<int>(row_top));
        } else if (row_bottom > view_bottom) {
            vscroll->setValue(static_cast<int>(row_bottom - vscroll->pageStep()));
        }
    }

    std::optional<size_t> indexForPosition(const QPointF &position) const {
        if (position.y() < 0.0) {
            return std::nullopt;
        }

        const size_t row = static_cast<size_t>(position.y() / row_height_);
        if (row >= rows_) {
            return std::nullopt;
        }

        auto index_for_column = [&](double x, double start_x, double slot_width, double active_width, int columns_per_byte)
            -> std::optional<size_t> {
            const double local_x = x - start_x;
            if (local_x < 0.0) {
                return std::nullopt;
            }

            const int slot = static_cast<int>(local_x / slot_width);
            if (slot < 0) {
                return std::nullopt;
            }

            const int byte_in_row = slot / columns_per_byte;
            if (byte_in_row < 0 || byte_in_row >= static_cast<int>(kBytesPerRow)) {
                return std::nullopt;
            }

            const double slot_offset = std::fmod(local_x, slot_width);
            if (slot_offset > active_width) {
                return std::nullopt;
            }

            const size_t index = row * kBytesPerRow + static_cast<size_t>(byte_in_row);
            if (index >= memory_size_) {
                return std::nullopt;
            }
            return index;
        };

        if (const std::optional<size_t> hex_index =
                index_for_column(position.x(), hex_start_x_ - 2.0, hex_cell_width_, hex_highlight_width_, 3)) {
            return hex_index;
        }

        if (const std::optional<size_t> ascii_index =
                index_for_column(position.x(), ascii_start_x_ - 1.0, ascii_cell_width_, ascii_highlight_width_, 1)) {
            return ascii_index;
        }

        return std::nullopt;
    }

    QScrollBar *verticalScrollBar() const {
        QWidget *p = parentWidget();
        while (p) {
            QScrollArea *sa = qobject_cast<QScrollArea *>(p);
            if (sa) return sa->verticalScrollBar();
            p = p->parentWidget();
        }
        return nullptr;
    }

    QWidget *viewport() {
        QWidget *p = parentWidget();
        while (p) {
            QScrollArea *sa = qobject_cast<QScrollArea *>(p);
            if (sa) return sa->viewport();
            p = p->parentWidget();
        }
        return this;
    }

    size_t memory_size_;
    std::function<bool(size_t, void *, size_t)> read_memory_;
    std::function<bool(size_t, const void *, size_t)> write_memory_;
    std::atomic<bool> &open_flag_;

    QTimer *timer_;

    const size_t rows_;
    std::vector<uint8_t> last_seen_;
    std::vector<double> changed_at_;
    std::vector<uint8_t> match_mask_;
    std::vector<AnnotationColorPoint> annotation_points_;
    std::vector<uint8_t> selection_mask_;
    std::vector<size_t> matches_;
    std::vector<uint8_t> visible_cache_;

    size_t visible_begin_ = 0;
    size_t visible_end_ = 0;
    size_t active_match_index_ = 0;
    size_t selection_anchor_ = std::numeric_limits<size_t>::max();
    size_t selection_drag_anchor_ = std::numeric_limits<size_t>::max();
    std::vector<size_t> selected_indices_;
    bool drag_selecting_ = false;
    bool logged_first_refresh_ = false;
    bool logged_first_paint_ = false;

    QFont font_;
    double char_width_;
    double row_height_;
    double baseline_y_;
    double address_x_;
    double hex_cell_width_;
    double ascii_cell_width_;
    double hex_start_x_;
    double ascii_start_x_;
    double content_width_;
    double hex_highlight_width_;
    double ascii_highlight_width_;

    void clearSelection() {
        std::fill(selection_mask_.begin(), selection_mask_.end(), 0);
        selected_indices_.clear();
    }

    void setSingleSelection(size_t index) {
        clearSelection();
        selection_mask_[index] = 1;
        selected_indices_.push_back(index);
        selection_anchor_ = index;
        notifySelectionChanged();
    }

    void setRangeSelection(size_t first, size_t last) {
        clearSelection();
        const size_t begin = std::min(first, last);
        const size_t end = std::max(first, last);
        for (size_t index = begin; index <= end; ++index) {
            if (index >= memory_size_) {
                break;
            }
            selection_mask_[index] = 1;
            selected_indices_.push_back(index);
        }
        selection_anchor_ = first;
        notifySelectionChanged();
        update();
    }

    void notifySelectionChanged() {
        if (onSelectionChanged) {
            onSelectionChanged(selected_indices_);
        }
    }
};

class MemViewerWindow : public QMainWindow {
public:
    struct NoteTabState {
        QWidget *page = nullptr;
        QLabel *selection_label = nullptr;
        QLabel *file_label = nullptr;
        NotePreview *preview = nullptr;
        QTextEdit *editor = nullptr;
        QPushButton *color_button = nullptr;
        QPushButton *clear_button = nullptr;
        AnnotationStore store;
        AnnotationStore::ResolvedAnnotation active_annotation;
        QColor current_color = kDefaultAnnotationColor;
        std::vector<AnnotationStore::ResolvedAnnotation> search_matches;
        int active_search_match = -1;
        QString last_search_query;
    };

    MemViewerWindow(
        size_t memory_size,
        std::function<bool(size_t, void *, size_t)> read_memory,
        std::function<bool(size_t, const void *, size_t)> write_memory,
        std::atomic<bool> &open_flag)
        : open_flag_(open_flag) {
        const double constructor_start = mem_viewer_now_seconds();
        
        setWindowTitle("Memory Viewer");
        resize(900, 680);
        setAttribute(Qt::WA_DeleteOnClose);
        createMenus();

        QWidget *central = new QWidget(this);
        setCentralWidget(central);

        QHBoxLayout *root_layout = new QHBoxLayout(central);
        root_layout->setContentsMargins(8, 8, 8, 8);
        root_layout->setSpacing(10);

        QWidget *main_panel = new QWidget();
        QVBoxLayout *main_layout = new QVBoxLayout(main_panel);
        main_layout->setContentsMargins(0, 0, 0, 0);
        main_layout->setSpacing(0);

        QWidget *viewer_panel = new QWidget();
        QHBoxLayout *viewer_layout = new QHBoxLayout(viewer_panel);
        viewer_layout->setContentsMargins(0, 0, 0, 0);
        viewer_layout->setSpacing(4);

        scroll_area_ = new QScrollArea();
        scroll_area_->setWidgetResizable(true);
        scroll_area_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        scroll_area_->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        note_scroll_bar_ = new NoteScrollBar(viewer_panel);
        note_scroll_bar_->setMemorySize(memory_size);
        viewer_layout->addWidget(scroll_area_);
        viewer_layout->addWidget(note_scroll_bar_);
        main_layout->addWidget(viewer_panel);

        QWidget *side_panel = new QWidget();
        side_panel->setFixedWidth(260);
        QVBoxLayout *side_layout = new QVBoxLayout(side_panel);
        side_layout->setSpacing(8);
        QTabWidget *side_tabs = new QTabWidget();

        QWidget *inspect_tab = new QWidget();
        QVBoxLayout *inspect_layout = new QVBoxLayout(inspect_tab);
        inspect_layout->setContentsMargins(0, 0, 0, 0);
        inspect_layout->setSpacing(8);

        auto_refresh_ = new QCheckBox("Auto refresh");
        auto_refresh_->setChecked(!mem_viewer_auto_refresh_forced_off());
        if (mem_viewer_auto_refresh_forced_off()) {
            auto_refresh_->setEnabled(false);
            auto_refresh_->setToolTip(QStringLiteral("Auto refresh is disabled for this viewer mode."));
        }

        refresh_button_ = new QPushButton("Refresh");
        connect(refresh_button_, &QPushButton::clicked, this, [this]() {
            if (viewer_widget_) {
                viewer_widget_->refreshVisibleBytes();
            }
        });

        QFrame *refresh_frame = new QFrame();
        refresh_frame->setFrameStyle(QFrame::Box | QFrame::Raised);
        QVBoxLayout *refresh_layout = new QVBoxLayout(refresh_frame);
        refresh_layout->setContentsMargins(8, 8, 8, 8);
        refresh_layout->setSpacing(6);
        refresh_layout->addWidget(auto_refresh_);
        refresh_layout->addWidget(refresh_button_);
        inspect_layout->addWidget(refresh_frame);

        QFrame *goto_frame = new QFrame();
        goto_frame->setFrameStyle(QFrame::Box | QFrame::Raised);
        QVBoxLayout *goto_layout = new QVBoxLayout(goto_frame);
        goto_layout->setContentsMargins(8, 8, 8, 8);
        goto_layout->setSpacing(6);
        goto_layout->addWidget(new QLabel("Go to position"));
        goto_entry_ = new QLineEdit();
        goto_entry_->setPlaceholderText("0x1234 or 4660");
        goto_layout->addWidget(goto_entry_);
        goto_button_ = new QPushButton("Go");
        goto_layout->addWidget(goto_button_);
        inspect_layout->addWidget(goto_frame);

        connect(goto_entry_, &QLineEdit::returnPressed, this, [this]() {
            goToPosition();
        });
        connect(goto_button_, &QPushButton::clicked, this, [this]() {
            goToPosition();
        });

        QFrame *edit_frame = new QFrame();
        edit_frame->setFrameStyle(QFrame::Box | QFrame::Raised);
        QVBoxLayout *edit_layout = new QVBoxLayout(edit_frame);
        edit_layout->setContentsMargins(8, 8, 8, 8);
        edit_layout->setSpacing(6);

        edit_layout->addWidget(new QLabel("Selected byte"));
        edit_entry_ = new QLineEdit();
        edit_entry_->setPlaceholderText("Selected byte: hex or decimal");
        edit_layout->addWidget(edit_entry_);

        apply_button_ = new QPushButton("Write byte");
        connect(apply_button_, &QPushButton::clicked, this, [this]() {
            applyEdit();
        });
        edit_layout->addWidget(apply_button_);

        connect(edit_entry_, &QLineEdit::returnPressed, this, [this]() {
            applyEdit();
        });

        inspect_layout->addWidget(edit_frame);
        inspect_layout->addStretch();
        side_tabs->addTab(inspect_tab, "Inspect");

        QWidget *search_tab = new QWidget();
        QVBoxLayout *search_tab_layout = new QVBoxLayout(search_tab);
        search_tab_layout->setContentsMargins(0, 0, 0, 0);
        search_tab_layout->setSpacing(8);

        QFrame *search_frame = new QFrame();
        search_frame->setFrameStyle(QFrame::Box | QFrame::Raised);
        QVBoxLayout *search_layout = new QVBoxLayout(search_frame);
        search_layout->setContentsMargins(8, 8, 8, 8);
        search_layout->setSpacing(6);

        search_layout->addWidget(new QLabel("Value"));
        search_entry_ = new QLineEdit();
        search_layout->addWidget(search_entry_);

        search_layout->addWidget(new QLabel("Format"));
        format_combo_ = new QComboBox();
        format_combo_->addItem("Hex");
        format_combo_->addItem("Decimal");
        search_layout->addWidget(format_combo_);

        search_layout->addWidget(new QLabel("Bytes"));
        width_combo_ = new QComboBox();
        width_combo_->addItem("1");
        width_combo_->addItem("2");
        width_combo_->addItem("4");
        width_combo_->addItem("8");
        search_layout->addWidget(width_combo_);

        search_layout->addWidget(new QLabel("Endian"));
        endian_combo_ = new QComboBox();
        endian_combo_->addItem("Little");
        endian_combo_->addItem("Big");
        search_layout->addWidget(endian_combo_);

        QHBoxLayout *search_nav = new QHBoxLayout();
        prev_button_ = new QPushButton("Prev");
        next_button_ = new QPushButton("Next");
        search_nav->addWidget(prev_button_);
        search_nav->addWidget(next_button_);
        search_layout->addLayout(search_nav);
        search_tab_layout->addWidget(search_frame);
        search_tab_layout->addStretch();
        side_tabs->addTab(search_tab, "Search");

        connect(search_entry_, &QLineEdit::returnPressed, this, [this]() {
            rebuildSearch();
        });
        connect(format_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]() {
            rebuildSearch();
        });
        connect(width_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]() {
            rebuildSearch();
        });
        connect(endian_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]() {
            rebuildSearch();
        });
        connect(prev_button_, &QPushButton::clicked, this, [this]() {
            if (viewer_widget_) {
                if (viewer_widget_->getMatchCount() == 0 || searchNeedsRebuild()) {
                    rebuildSearch();
                }
                viewer_widget_->navigateMatch(-1);
            }
        });
        connect(next_button_, &QPushButton::clicked, this, [this]() {
            if (viewer_widget_) {
                if (viewer_widget_->getMatchCount() == 0 || searchNeedsRebuild()) {
                    rebuildSearch();
                }
                viewer_widget_->navigateMatch(1);
            }
        });

        QWidget *notes_tab = new QWidget();
        QVBoxLayout *notes_layout = new QVBoxLayout(notes_tab);
        notes_layout->setContentsMargins(0, 0, 0, 0);
        notes_layout->setSpacing(8);

        QFrame *notes_search_frame = new QFrame();
        notes_search_frame->setFrameStyle(QFrame::Box | QFrame::Raised);
        QVBoxLayout *notes_search_layout = new QVBoxLayout(notes_search_frame);
        notes_search_layout->setContentsMargins(8, 8, 8, 8);
        notes_search_layout->setSpacing(6);
        notes_search_layout->addWidget(new QLabel("Search notes"));
        notes_search_entry_ = new QLineEdit();
        notes_search_entry_->setPlaceholderText("Text in note");
        notes_search_layout->addWidget(notes_search_entry_);
        QHBoxLayout *notes_search_nav = new QHBoxLayout();
        notes_prev_button_ = new QPushButton("Prev");
        notes_next_button_ = new QPushButton("Next");
        notes_search_nav->addWidget(notes_prev_button_);
        notes_search_nav->addWidget(notes_next_button_);
        notes_search_layout->addLayout(notes_search_nav);
        notes_layout->addWidget(notes_search_frame);

        notes_file_tabs_ = new QTabWidget();
        notes_layout->addWidget(notes_file_tabs_);
        notes_layout->addStretch();
        side_tabs->addTab(notes_tab, "Notes");

        connect(notes_search_entry_, &QLineEdit::returnPressed, this, [this]() {
            navigateNoteSearch(1, true);
        });
        connect(notes_search_entry_, &QLineEdit::textChanged, this, [this](const QString &) {
            if(NoteTabState *state = currentNoteTabState()) {
                state->search_matches.clear();
                state->active_search_match = -1;
                state->last_search_query.clear();
            }
            updateNotesSearchUi();
        });
        connect(notes_prev_button_, &QPushButton::clicked, this, [this]() {
            navigateNoteSearch(-1, false);
        });
        connect(notes_next_button_, &QPushButton::clicked, this, [this]() {
            navigateNoteSearch(1, false);
        });

        side_layout->addWidget(side_tabs);

        QFrame *status_frame = new QFrame();
        status_frame->setFrameStyle(QFrame::Box | QFrame::Raised);
        QVBoxLayout *status_layout = new QVBoxLayout(status_frame);
        status_layout->setContentsMargins(8, 8, 8, 8);
        status_layout->setSpacing(6);

        status_label_ = new QLabel();
        status_label_->setWordWrap(true);
        status_layout->addWidget(status_label_);

        side_layout->addWidget(status_frame);

        side_layout->addStretch();

        root_layout->addWidget(main_panel, 1);
        root_layout->addWidget(side_panel);

        viewer_widget_ = new MemViewerWidget(memory_size, read_memory, write_memory, open_flag, scroll_area_);
        scroll_area_->setWidget(viewer_widget_);
        connect(scroll_area_->verticalScrollBar(), &QScrollBar::valueChanged, this, [this](int) {
            if (viewer_widget_) {
                viewer_widget_->refreshVisibleBytes();
            }
        });
        note_scroll_bar_->onAnnotatedRowActivated = [this](size_t row) {
            if (scroll_area_ == nullptr) {
                return;
            }
            QScrollBar *scroll_bar = scroll_area_->verticalScrollBar();
            if (scroll_bar == nullptr) {
                return;
            }
            const size_t row_count = viewer_widget_ == nullptr ? 0 : ((viewer_widget_->getMemorySize() + kBytesPerRow - 1) / kBytesPerRow);
            if (row_count <= 1 || scroll_bar->maximum() <= scroll_bar->minimum()) {
                scroll_bar->setValue(scroll_bar->minimum());
                return;
            }
            const double ratio = static_cast<double>(std::min(row, row_count - 1)) / static_cast<double>(row_count - 1);
            const double value = static_cast<double>(scroll_bar->minimum()) + ratio * static_cast<double>(scroll_bar->maximum() - scroll_bar->minimum());
            scroll_bar->setValue(std::clamp(static_cast<int>(std::round(value)), scroll_bar->minimum(), scroll_bar->maximum()));
        };

        viewer_widget_->onSelectionChanged = [this](const std::vector<size_t> &selection) {
            onSelectionChanged(selection);
        };
        viewer_widget_->onSearchStatusUpdated = [this]() {
            updateStatus();
        };

        connect(auto_refresh_, &QCheckBox::toggled, viewer_widget_, [this](bool checked) {
            if (viewer_widget_) {
                viewer_widget_->setAutoRefresh(checked);
            }
        });
        if (viewer_widget_ && mem_viewer_auto_refresh_forced_off()) {
            viewer_widget_->setAutoRefresh(false);
        }

        connect(notes_file_tabs_, &QTabWidget::currentChanged, this, [this](int) {
            refreshAnnotationHighlights();
            updateAnnotationUi();
            updateStatus();
        });

        if (!mem_viewer_static_file_mode()) {
            loadNoteFilesFromEnvironment();
            note_files_loaded_ = true;
            refreshAnnotationHighlights();
        }
        updateStatus(std::numeric_limits<size_t>::max(), 0);
        updateAnnotationUi();
        mem_viewer_debug_log("window constructed in %.3f s%s",
            mem_viewer_now_seconds() - constructor_start,
            mem_viewer_static_file_mode() ? " (notes deferred)" : "");
    }

    ~MemViewerWindow() override {
        open_flag_.store(false, std::memory_order_relaxed);
    }

    void loadDeferredNoteFiles() {
        if (note_files_loaded_) {
            return;
        }
        note_files_loaded_ = true;
        const double start = mem_viewer_now_seconds();
        mem_viewer_debug_log("loading deferred note files");
        loadNoteFilesFromEnvironment();
        refreshAnnotationHighlights();
        updateStatus();
        updateAnnotationUi();
        mem_viewer_debug_log("deferred note loading finished in %.3f s", mem_viewer_now_seconds() - start);
    }

private:
    void createMenus() {
        QMenu *file_menu = menuBar()->addMenu("&File");

        QAction *load_annotations = file_menu->addAction("Load Notes File...");
        connect(load_annotations, &QAction::triggered, this, [this]() {
            QFileDialog dialog(this, QStringLiteral("Load Notes File"));
            dialog.setAcceptMode(QFileDialog::AcceptOpen);
            dialog.setFileMode(QFileDialog::ExistingFile);
            dialog.setNameFilter(QStringLiteral("JSON Files (*.json);;All Files (*)"));
            dialog.setOption(QFileDialog::DontUseNativeDialog, true);
            if (NoteTabState *state = currentNoteTabState()) {
                if (state->store.hasFilePath()) {
                    dialog.selectFile(state->store.filePath());
                }
            }

            if (dialog.exec() != QDialog::Accepted) {
                return;
            }

            const QStringList selected_files = dialog.selectedFiles();
            if (selected_files.isEmpty()) {
                return;
            }

            selectAnnotationFile(selected_files.front());
        });

        QAction *create_annotations = file_menu->addAction("New Notes File...");
        connect(create_annotations, &QAction::triggered, this, [this]() {
            QFileDialog dialog(this, QStringLiteral("Select Annotation File"));
            dialog.setAcceptMode(QFileDialog::AcceptSave);
            dialog.setFileMode(QFileDialog::AnyFile);
            dialog.setNameFilter(QStringLiteral("JSON Files (*.json);;All Files (*)"));
            dialog.setOption(QFileDialog::DontUseNativeDialog, true);
            if (NoteTabState *state = currentNoteTabState()) {
                if (state->store.hasFilePath()) {
                    dialog.selectFile(state->store.filePath());
                }
            }

            if (dialog.exec() != QDialog::Accepted) {
                return;
            }

            const QStringList selected_files = dialog.selectedFiles();
            if (selected_files.isEmpty()) {
                return;
            }
            selectAnnotationFile(selected_files.front());
        });

        QMenu *edit_menu = menuBar()->addMenu("&Edit");
        QAction *copy_selection = edit_menu->addAction("Copy Selection");
        copy_selection->setShortcut(QKeySequence::Copy);
        connect(copy_selection, &QAction::triggered, this, [this]() {
            copySelectionToClipboard();
        });
        addAction(copy_selection);
    }

    void selectAnnotationFile(const QString &path) {
        if (path.isEmpty()) {
            return;
        }
        const double start = mem_viewer_now_seconds();
        mem_viewer_debug_log("selectAnnotationFile begin path=%s", path.toLocal8Bit().constData());

        if (int existing_index = findNoteTabByPath(path); existing_index >= 0) {
            notes_file_tabs_->setCurrentIndex(existing_index);
            updateAnnotationUi();
            updateStatus();
            mem_viewer_debug_log("selectAnnotationFile reused existing tab path=%s in %.3f s",
                path.toLocal8Bit().constData(),
                mem_viewer_now_seconds() - start);
            return;
        }

        auto note_tab = std::make_unique<NoteTabState>();
        note_tab->page = new QWidget();
        QVBoxLayout *page_layout = new QVBoxLayout(note_tab->page);
        page_layout->setContentsMargins(0, 0, 0, 0);
        page_layout->setSpacing(0);

        QFrame *annotation_frame = new QFrame();
        annotation_frame->setFrameStyle(QFrame::Box | QFrame::Raised);
        QVBoxLayout *annotation_layout = new QVBoxLayout(annotation_frame);
        annotation_layout->setContentsMargins(8, 8, 8, 8);
        annotation_layout->setSpacing(6);

        note_tab->selection_label = new QLabel("No bytes selected");
        note_tab->selection_label->setWordWrap(true);
        annotation_layout->addWidget(note_tab->selection_label);

        note_tab->file_label = new QLabel();
        note_tab->file_label->setWordWrap(true);
        annotation_layout->addWidget(note_tab->file_label);

        note_tab->preview = new NotePreview();
        note_tab->preview->setPlaceholderText("Ctrl+click a rendered link to jump to its target");
        note_tab->preview->setMinimumHeight(72);
        note_tab->preview->setEnabled(false);
        annotation_layout->addWidget(note_tab->preview);

        note_tab->editor = new QTextEdit();
        note_tab->editor->setPlaceholderText("Select one or more bytes and type notes here");
        note_tab->editor->setEnabled(false);
        annotation_layout->addWidget(note_tab->editor);

        note_tab->color_button = new QPushButton("Highlight color");
        note_tab->color_button->setEnabled(false);
        annotation_layout->addWidget(note_tab->color_button);

        note_tab->clear_button = new QPushButton("Clear selected annotations");
        note_tab->clear_button->setEnabled(false);
        annotation_layout->addWidget(note_tab->clear_button);

        page_layout->addWidget(annotation_frame);

        QString error_message;
        if (!note_tab->store.selectFile(path, &error_message)) {
            delete note_tab->page;
            QMessageBox::warning(this, QStringLiteral("Annotations"), error_message);
            return;
        }

        note_tab->current_color = kDefaultAnnotationColor;

        connect(note_tab->editor, &QTextEdit::textChanged, this, [this, raw = note_tab.get()]() {
            onAnnotationEdited(raw);
        });
        note_tab->preview->onLinkActivated = [this](const QUrl &url) {
            if(url.scheme() == QStringLiteral("jump")) {
                jumpToNoteLink(url.path().isEmpty() ? url.toString().mid(5) : url.path());
            }
        };
        connect(note_tab->color_button, &QPushButton::clicked, this, [this, raw = note_tab.get()]() {
            chooseAnnotationColor(raw);
        });
        connect(note_tab->clear_button, &QPushButton::clicked, this, [this, raw = note_tab.get()]() {
            clearSelectedAnnotations(raw);
        });

        const QFileInfo file_info(path);
        const QString tab_name = file_info.completeBaseName().isEmpty()
            ? (file_info.fileName().isEmpty() ? path : file_info.fileName())
            : file_info.completeBaseName();
        const int new_index = notes_file_tabs_->addTab(note_tab->page, tab_name);
        note_tabs_.push_back(std::move(note_tab));
        notes_file_tabs_->setCurrentIndex(new_index);

        refreshAnnotationHighlights();
        updateAnnotationUi();
        updateStatus();
        mem_viewer_debug_log("selectAnnotationFile finished path=%s in %.3f s",
            path.toLocal8Bit().constData(),
            mem_viewer_now_seconds() - start);
    }

    void loadNoteFilesFromEnvironment() {
        const char *env_value = std::getenv("MEM_VIEWER_NOTES");
        if (env_value == nullptr || env_value[0] == '\0') {
            return;
        }

        const std::vector<QString> note_paths = split_note_file_list(QString::fromLocal8Bit(env_value));
        mem_viewer_debug_log("loading %zu note file(s) from environment", note_paths.size());
        for (const QString &path : note_paths) {
            selectAnnotationFile(path);
        }
    }

    void rebuildSearch() {
        if (!viewer_widget_) return;
        
        const std::string search_text = search_entry_->text().toStdString();
        const SearchFormat format = format_combo_->currentIndex() == 0 ? SearchFormat::Hex : SearchFormat::Decimal;
        const EndianMode endian = endian_combo_->currentIndex() == 0 ? EndianMode::Little : EndianMode::Big;
        const size_t width = static_cast<size_t>(width_combo_->currentText().toULongLong());
        
        viewer_widget_->rebuildSearch(search_text, format, endian, width);
        last_search_signature_ = currentSearchSignature();
    }

    std::string currentSearchSignature() const {
        std::ostringstream ss;
        ss << search_entry_->text().toStdString() << '|'
           << format_combo_->currentIndex() << '|'
           << width_combo_->currentText().toStdString() << '|'
           << endian_combo_->currentIndex();
        return ss.str();
    }

    bool searchNeedsRebuild() const {
        return currentSearchSignature() != last_search_signature_;
    }

    void applyEdit() {
        if (!viewer_widget_) return;
        
        uint8_t value = 0;
        const std::string text = edit_entry_->text().toStdString();
        if (!parse_byte_value(text, value)) {
            return;
        }
        viewer_widget_->applyEdit(value);
    }

    void goToPosition() {
        if (!viewer_widget_) {
            return;
        }

        size_t index = 0;
        if (!parse_memory_position(goto_entry_->text().toStdString(), viewer_widget_->getMemorySize(), index)) {
            QMessageBox::warning(
                this,
                QStringLiteral("Go to position"),
                QStringLiteral("Enter a valid byte offset within 0 to %1.")
                    .arg(viewer_widget_->getMemorySize() == 0 ? 0 : viewer_widget_->getMemorySize() - 1));
            return;
        }

        viewer_widget_->jumpToIndex(index);
    }

    void jumpToNoteLink(const QString &targetText) {
        if(viewer_widget_ == nullptr) {
            return;
        }

        size_t index = 0;
        if(!parse_memory_position(targetText.toStdString(), viewer_widget_->getMemorySize(), index)) {
            mem_viewer_debug_log("note link target parse failed: %s", targetText.toLocal8Bit().constData());
            return;
        }
        viewer_widget_->jumpToIndex(index);
    }

    void navigateToNoteMatch(NoteTabState &state) {
        if(viewer_widget_ == nullptr || state.active_search_match < 0 ||
           state.active_search_match >= static_cast<int>(state.search_matches.size())) {
            return;
        }

        const AnnotationStore::ResolvedAnnotation &match = state.search_matches[static_cast<size_t>(state.active_search_match)];
        if(match.positions.empty()) {
            return;
        }
        viewer_widget_->jumpToIndex(match.positions.front());
    }

    void navigateNoteSearch(int direction, bool rebuildMatches) {
        NoteTabState *state = currentNoteTabState();
        if(state == nullptr) {
            return;
        }

        const QString query = notes_search_entry_ == nullptr ? QString() : notes_search_entry_->text();
        if(rebuildMatches || query != state->last_search_query) {
            state->search_matches = state->store.searchNotes(query);
            state->active_search_match = state->search_matches.empty() ? -1 : (direction < 0 ? static_cast<int>(state->search_matches.size()) - 1 : 0);
            state->last_search_query = query;
        } else if(!state->search_matches.empty()) {
            const int count = static_cast<int>(state->search_matches.size());
            if(state->active_search_match < 0 || state->active_search_match >= count) {
                state->active_search_match = 0;
            } else if(direction < 0) {
                state->active_search_match = (state->active_search_match + count - 1) % count;
            } else {
                state->active_search_match = (state->active_search_match + 1) % count;
            }
        }

        navigateToNoteMatch(*state);
        updateStatus();
    }

    void onSelectionChanged(const std::vector<size_t> &selection) {
        const double start = mem_viewer_now_seconds();
        current_selection_ = selection;

        const size_t single_index = viewer_widget_ ? viewer_widget_->getSelectedIndex() : std::numeric_limits<size_t>::max();
        if (single_index < std::numeric_limits<size_t>::max()) {
            const uint8_t value = viewer_widget_->getSelectedValue();
            char text[8];
            std::snprintf(text, sizeof(text), "%02X", value);
            edit_entry_->setText(text);
        } else {
            edit_entry_->clear();
        }

        edit_entry_->setEnabled(selection.size() == 1);
        apply_button_->setEnabled(selection.size() == 1);
        updateAnnotationUi();
        updateStatus();
        mem_viewer_debug_log("onSelectionChanged bytes=%zu elapsed=%.6f s",
            selection.size(),
            mem_viewer_now_seconds() - start);
    }

    void updateAnnotationUi() {
        if (notes_file_tabs_ == nullptr) {
            return;
        }

        if (NoteTabState *state = currentNoteTabState()) {
            updateAnnotationUiForTab(*state);
        }
        updateNotesSearchUi();
    }

    void onAnnotationEdited(NoteTabState *state) {
        if (state == nullptr) {
            return;
        }

        if(state->preview != nullptr) {
            state->preview->setHtml(note_text_to_html(state->editor->toPlainText()));
        }

        if (current_selection_.empty()) {
            return;
        }

        if (!state->store.hasFilePath()) {
            updateStatus();
            return;
        }

        const std::vector<size_t> target_positions = state->active_annotation.isValid() ? state->active_annotation.positions : current_selection_;
        QString error_message;
        if (!state->store.setAnnotation(target_positions, state->editor->toPlainText().toStdString(), state->current_color, &error_message)) {
            QMessageBox::warning(this, QStringLiteral("Annotations"), error_message);
            return;
        }
        state->active_annotation = state->store.resolveForSelection(current_selection_);
        refreshAnnotationHighlights();
        updateAnnotationUiForTab(*state);
        updateStatus();
    }

    void chooseAnnotationColor(NoteTabState *state) {
        if (state == nullptr) {
            return;
        }

        if (current_selection_.empty()) {
            return;
        }

        const QColor chosen = QColorDialog::getColor(state->current_color, this, QStringLiteral("Choose Note Highlight Color"));
        if (!chosen.isValid()) {
            return;
        }

        state->current_color = chosen;
        updateAnnotationColorButton(*state);

        if (state->store.hasFilePath()) {
            onAnnotationEdited(state);
        }
    }

    void clearSelectedAnnotations(NoteTabState *state) {
        if (state == nullptr || current_selection_.empty() || !state->store.hasFilePath()) {
            return;
        }

        QString error_message;
        if (!state->store.clearAnnotationPositions(current_selection_, &error_message)) {
            QMessageBox::warning(this, QStringLiteral("Annotations"), error_message);
            return;
        }

        refreshAnnotationHighlights();
        updateAnnotationUiForTab(*state);
        updateAnnotationUi();
        updateStatus();
    }

    void refreshAnnotationHighlights() {
        std::vector<AnnotationColorPoint> annotated_positions;
        if (NoteTabState *state = currentNoteTabState()) {
            annotated_positions = state->store.annotatedPositions();
        }
        if (viewer_widget_) {
            viewer_widget_->setAnnotatedPositions(annotated_positions);
        }
        if (note_scroll_bar_) {
            note_scroll_bar_->setAnnotatedPositions(annotated_positions);
        }
    }

    void copySelectionToClipboard() {
        if (!viewer_widget_) {
            return;
        }

        const std::string text = viewer_widget_->selectedContentText();
        if (text.empty()) {
            return;
        }

        if (QClipboard *clipboard = QApplication::clipboard()) {
            clipboard->setText(QString::fromStdString(text));
        }
    }

    void updateAnnotationColorButton(NoteTabState &state) {
        const QColor color = state.current_color.isValid() ? state.current_color : kDefaultAnnotationColor;
        state.color_button->setText(QStringLiteral("Highlight: %1").arg(color.name(QColor::HexRgb)));
        state.color_button->setStyleSheet(annotation_color_button_style(color));
    }

    void updateAnnotationUiForTab(NoteTabState &state) {
        const double start = mem_viewer_now_seconds();
        const QString selection_text = selection_summary_text(current_selection_);
        if (state.selection_label->text() != selection_text) {
            state.selection_label->setText(selection_text);
        }
        const QString file_text = state.store.hasFilePath()
            ? QStringLiteral("Annotation file: %1").arg(state.store.filePath())
            : QStringLiteral("Annotation file: not selected");
        if (state.file_label->text() != file_text) {
            state.file_label->setText(file_text);
        }

        const bool can_edit = !current_selection_.empty();
        state.editor->setEnabled(can_edit);
        state.color_button->setEnabled(can_edit);
        state.clear_button->setEnabled(can_edit && state.store.hasFilePath());

        state.active_annotation = state.store.resolveForSelection(current_selection_);
        const QString note = QString::fromStdString(state.active_annotation.note);
        if (state.preview != nullptr) {
            state.preview->setEnabled(!note.isEmpty());
            state.preview->setHtml(note_text_to_html(note));
        }
        if (state.editor->toPlainText() != note) {
            const QSignalBlocker blocker(state.editor);
            state.editor->setPlainText(note);
        }
        state.current_color = state.active_annotation.isValid() ? state.active_annotation.color : kDefaultAnnotationColor;
        updateAnnotationColorButton(state);
        mem_viewer_debug_log("updateAnnotationUiForTab selection=%zu note_bytes=%zu elapsed=%.6f s",
            current_selection_.size(),
            note.size(),
            mem_viewer_now_seconds() - start);
    }

    void updateNotesSearchUi() {
        NoteTabState *state = currentNoteTabState();
        const bool has_query = notes_search_entry_ != nullptr && !notes_search_entry_->text().trimmed().isEmpty();
        const bool has_matches = state != nullptr && !state->search_matches.empty();
        if(notes_prev_button_ != nullptr) {
            notes_prev_button_->setEnabled(has_query && has_matches);
        }
        if(notes_next_button_ != nullptr) {
            notes_next_button_->setEnabled(has_query && has_matches);
        }
    }

    int findNoteTabByPath(const QString &path) const {
        for (size_t i = 0; i < note_tabs_.size(); ++i) {
            if (note_tabs_[i]->store.filePath() == path) {
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    NoteTabState *currentNoteTabState() const {
        if (notes_file_tabs_ == nullptr) {
            return nullptr;
        }
        const int index = notes_file_tabs_->currentIndex();
        if (index < 0 || index >= static_cast<int>(note_tabs_.size())) {
            return nullptr;
        }
        return note_tabs_[static_cast<size_t>(index)].get();
    }

    void updateStatus(size_t index = std::numeric_limits<size_t>::max(), uint8_t value = 0) {
        if (!viewer_widget_) return;
        
        if (index >= viewer_widget_->getMemorySize()) {
            if (current_selection_.size() == 1) {
                index = current_selection_.front();
                value = viewer_widget_->getSelectedValue();
            }
        }

        if (index >= viewer_widget_->getMemorySize()) {
            std::ostringstream ss;
            ss << "Buffer size: " << viewer_widget_->getMemorySize() << " bytes";
            const size_t match_count = viewer_widget_->getMatchCount();
            if (match_count > 0) {
                ss << " | Matches: " << match_count;
            }
            if (!current_selection_.empty()) {
                ss << " | Selected: " << current_selection_.size();
            }
            if (!note_tabs_.empty()) {
                ss << " | Notes: autosaved";
            } else if (!current_selection_.empty()) {
                ss << " | Load a notes file in File menu";
            }
            status_label_->setText(QString::fromStdString(ss.str()));
            return;
        }

        char text[256];
        std::snprintf(
            text,
            sizeof(text),
            "Selected: 0x%08zx | hex=%02X | dec=%u | Matches=%zu",
            index,
            value,
            static_cast<unsigned>(value),
            viewer_widget_->getMatchCount());
        status_label_->setText(QString::fromLatin1(text));
    }

    std::atomic<bool> &open_flag_;
    QScrollArea *scroll_area_;
    MemViewerWidget *viewer_widget_;
    NoteScrollBar *note_scroll_bar_;
    QCheckBox *auto_refresh_;
    QPushButton *refresh_button_;
    QLineEdit *goto_entry_;
    QPushButton *goto_button_;
    QLineEdit *search_entry_;
    QComboBox *format_combo_;
    QComboBox *width_combo_;
    QComboBox *endian_combo_;
    QPushButton *prev_button_;
    QPushButton *next_button_;
    QLineEdit *edit_entry_;
    QPushButton *apply_button_;
    QLineEdit *notes_search_entry_;
    QPushButton *notes_prev_button_;
    QPushButton *notes_next_button_;
    QTabWidget *notes_file_tabs_;
    QLabel *status_label_;
    std::vector<std::unique_ptr<NoteTabState>> note_tabs_;
    std::vector<size_t> current_selection_;
    std::string last_search_signature_;
    bool note_files_loaded_ = false;
};

}  // namespace

int mem_viewer_run_gui(pid_t target_pid, uintptr_t target_address, size_t size) {
    prctl(PR_SET_PDEATHSIG, SIGTERM);
    mem_viewer_debug_log("starting Qt GUI target_pid=%ld address=0x%llx size=%zu",
        static_cast<long>(target_pid),
        static_cast<unsigned long long>(target_address),
        size);

    RemoteMemory memory(target_pid, target_address, size);
    
    int argc = 1;
    const char *argv[] = {"mem_viewer", nullptr};
    QApplication app(argc, const_cast<char **>(argv));
    
    mem_viewer_debug_log("QApplication created");
    std::atomic<bool> open_flag{false};
    
    auto *window = new MemViewerWindow(
        memory.size(),
        [&memory](size_t offset, void *buffer, size_t length) { return memory.read(offset, buffer, length); },
        [&memory](size_t offset, const void *buffer, size_t length) { return memory.write(offset, buffer, length); },
        open_flag);
    
    open_flag.store(true, std::memory_order_relaxed);
    window->show();
    if (mem_viewer_static_file_mode()) {
        QTimer::singleShot(50, window, [window]() { window->loadDeferredNoteFiles(); });
    }
    mem_viewer_debug_log("window shown");

    const int rc = app.exec();
    mem_viewer_debug_log("QApplication::exec() returned rc=%d", rc);
    return rc;
}

int mem_viewer_run_gui_shared(void *memory_ptr, size_t size) {
    mem_viewer_debug_log("starting shared Qt GUI memory=%p size=%zu", memory_ptr, size);

    LocalMemory memory(memory_ptr, size);
    
    int argc = 1;
    const char *argv[] = {"mem_viewer_shared", nullptr};
    QApplication app(argc, const_cast<char **>(argv));
    
    std::atomic<bool> open_flag{false};
    
    auto *window = new MemViewerWindow(
        memory.size(),
        [&memory](size_t offset, void *buffer, size_t length) { return memory.read(offset, buffer, length); },
        [&memory](size_t offset, const void *buffer, size_t length) { return memory.write(offset, buffer, length); },
        open_flag);
    
    open_flag.store(true, std::memory_order_relaxed);
    window->show();
    if (mem_viewer_static_file_mode()) {
        QTimer::singleShot(50, window, [window]() { window->loadDeferredNoteFiles(); });
    }
    mem_viewer_debug_log("shared window shown");

    const int rc = app.exec();
    mem_viewer_debug_log("shared QApplication::exec() returned rc=%d", rc);
    return rc;
}
