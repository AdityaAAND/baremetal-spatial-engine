#pragma once
#include <ostream>
#include <string>
#include <vector>

enum class ReadStatus {
    LINE,       // a line was entered
    CANCELLED,  // Ctrl-C: drop whatever is being typed
    END         // Ctrl-D on an empty line, or the terminal went away
};

// Minimal terminal line editing: cursor keys, history and the usual Ctrl shortcuts.
// Hand-written because the project has no dependencies. Stdin must be a terminal.
class LineEditor {
private:
    std::ostream& out;
    std::vector<std::string> history; // this session only

    void refresh(const std::string& prompt, const std::string& text, size_t cursor);

public:
    explicit LineEditor(std::ostream& output);
    ReadStatus read_line(const std::string& prompt, std::string& line);
};
