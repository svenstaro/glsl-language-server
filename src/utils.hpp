#pragma once

#include <optional>
#include <string>
#include <vector>

std::vector<std::string> split_string(const std::string& string_to_split, const std::string& pattern);

std::string trim_right(const std::string& s, const std::string& delimiters);

std::string trim_left(const std::string& s, const std::string& delimiters);

std::string trim(const std::string& s, const std::string& delimiters);

struct SourceFileLocation {
    /// Zero indexed line index
    int line;
    /// Zero indexed character index from the start of the line
    int character;
};

/// Returns the byte offset for the given character on the given line.
int find_position_offset(const char* text, int line, int character);

/// Given a byte offset into a file, returns the corresponding line and column.
SourceFileLocation find_source_location(const char* text, int offset);

/// Returns `true` if the character may start an identifier.
bool is_identifier_start_char(char c);

/// Returns `true` if the character may be part of an identifier.
bool is_identifier_char(char c);

/// Returns the offset in `text` where the last word started.
int get_last_word_start(const char* text, int offset);

/// Given an index inside a word, returns the index of the end of the word (ie.
/// one past the last character)
int get_word_end(const char* text, int start);

/// Open the file with the given name, and return its contents as a string.
std::optional<std::string> read_file_to_string(const char* path);

/// Given a file path, returns its URI
std::string make_path_uri(const std::string& path);

/// Returns a pointer into `haystack` with the `prefix` removed from the start.
/// If `haystack` does not begin with `prefix`, returns null.
const char* strip_prefix(const char* prefix, const char* haystack);

