#include "LineEditor.h"
#include <algorithm>
#include <cctype>
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

// How long a lone Esc waits for the rest of a key sequence, which arrives in one burst
static constexpr int ESCAPE_TIMEOUT_MS = 50;

static constexpr char KEY_CTRL_A = 1;
static constexpr char KEY_CTRL_C = 3;
static constexpr char KEY_CTRL_D = 4;
static constexpr char KEY_CTRL_E = 5;
static constexpr char KEY_BACKSPACE_ALT = 8;
static constexpr char KEY_CTRL_K = 11;
static constexpr char KEY_CTRL_U = 21;
static constexpr char KEY_ESCAPE = 27;
static constexpr char KEY_BACKSPACE = 127;

namespace {

// Raw mode lasts only while a line is being typed, so commands run and print on a normal
// terminal. ISIG stays off because a Ctrl-C signal would kill the process with the terminal
// still raw; it arrives as a key instead.
class RawMode {
private:
    termios saved;
    bool active = false;

public:
    RawMode() {
        if (tcgetattr(STDIN_FILENO, &saved) != 0) return;
        termios raw = saved;
        raw.c_lflag &= ~static_cast<tcflag_t>(ICANON | ECHO | ISIG | IEXTEN);
        raw.c_cc[VMIN] = 1;
        raw.c_cc[VTIME] = 0;
        active = tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == 0;
    }
    ~RawMode() {
        if (active) tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved);
    }
    bool ok() const { return active; }
};

bool read_byte(char& c) {
    return read(STDIN_FILENO, &c, 1) == 1;
}

bool byte_waiting() {
    pollfd fd{STDIN_FILENO, POLLIN, 0};
    return poll(&fd, 1, ESCAPE_TIMEOUT_MS) > 0;
}

size_t terminal_width() {
    winsize size{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0 && size.ws_col > 0) return size.ws_col;
    return 80;
}

enum class Key { NONE, LEFT, RIGHT, UP, DOWN, HOME, END, DELETE_KEY };

// Consumes a whole escape sequence, even an unknown one, so none of it lands in the command.
// A sequence is parameters followed by one final letter or ~, as in ESC [ 3 ~ or ESC [ 1 ; 5 C.
Key read_escape_sequence() {
    char kind;
    if (!byte_waiting() || !read_byte(kind)) return Key::NONE;
    if (kind != '[' && kind != 'O') return Key::NONE;

    std::string params;
    char final_byte;
    while (true) {
        if (!byte_waiting() || !read_byte(final_byte)) return Key::NONE;
        if (final_byte >= 0x40 && final_byte <= 0x7e) break;
        params += final_byte;
    }

    switch (final_byte) {
        case 'A': return Key::UP;
        case 'B': return Key::DOWN;
        case 'C': return Key::RIGHT;
        case 'D': return Key::LEFT;
        case 'H': return Key::HOME;
        case 'F': return Key::END;
        case '~':
            if (params == "3") return Key::DELETE_KEY;
            if (params == "1" || params == "7") return Key::HOME;
            if (params == "4" || params == "8") return Key::END;
            return Key::NONE;
        default:
            return Key::NONE;
    }
}

} // namespace

LineEditor::LineEditor(std::ostream& output) : out(output) {}

// A long command scrolls sideways instead of wrapping, because the \r redraw can only reach
// the current row. The view slides just far enough to keep the cursor on screen.
void LineEditor::refresh(const std::string& prompt, const std::string& text, size_t cursor) {
    const size_t width = terminal_width();
    size_t start = 0;
    while (prompt.size() + (cursor - start) >= width && start < cursor) ++start;

    const size_t room = width > prompt.size() ? width - prompt.size() - 1 : 0;
    out << "\r" << prompt << text.substr(start, room) << "\x1b[0K"
        << "\r\x1b[" << prompt.size() + (cursor - start) << "C" << std::flush;
}

ReadStatus LineEditor::read_line(const std::string& prompt, std::string& line) {
    RawMode raw;
    if (!raw.ok()) return ReadStatus::END;

    std::string text;
    std::string draft; // what was being typed before moving into history
    size_t cursor = 0;
    size_t history_index = history.size();

    auto show_history = [&](size_t index) {
        if (history_index == history.size()) draft = text;
        history_index = index;
        text = index == history.size() ? draft : history[index];
        cursor = text.size();
    };

    while (true) {
        refresh(prompt, text, cursor);

        char c;
        if (!read_byte(c)) {
            out << "\n" << std::flush;
            return ReadStatus::END;
        }

        Key key = Key::NONE;
        if (c == KEY_ESCAPE) key = read_escape_sequence();

        if (c == '\r' || c == '\n') {
            out << "\n" << std::flush;
            const bool blank = std::all_of(text.begin(), text.end(), [](unsigned char ch) { return std::isspace(ch) != 0; });
            if (!blank && (history.empty() || history.back() != text)) history.push_back(text);
            line = text;
            return ReadStatus::LINE;
        } else if (c == KEY_CTRL_C) {
            out << "^C\n" << std::flush;
            return ReadStatus::CANCELLED;
        } else if (c == KEY_CTRL_D && text.empty()) {
            out << "\n" << std::flush;
            return ReadStatus::END;
        } else if (c == KEY_BACKSPACE || c == KEY_BACKSPACE_ALT) {
            if (cursor > 0) text.erase(--cursor, 1);
        } else if (c == KEY_CTRL_D || key == Key::DELETE_KEY) {
            if (cursor < text.size()) text.erase(cursor, 1);
        } else if (key == Key::LEFT) {
            if (cursor > 0) --cursor;
        } else if (key == Key::RIGHT) {
            if (cursor < text.size()) ++cursor;
        } else if (c == KEY_CTRL_A || key == Key::HOME) {
            cursor = 0;
        } else if (c == KEY_CTRL_E || key == Key::END) {
            cursor = text.size();
        } else if (key == Key::UP) {
            if (history_index > 0) show_history(history_index - 1);
        } else if (key == Key::DOWN) {
            if (history_index < history.size()) show_history(history_index + 1);
        } else if (c == KEY_CTRL_K) {
            text.erase(cursor);
        } else if (c == KEY_CTRL_U) {
            text.erase(0, cursor);
            cursor = 0;
        } else if (c >= 32 && c < 127) {
            text.insert(cursor++, 1, c);
        }
    }
}
