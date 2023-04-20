#pragma once

#include <string>
#include <vector>

std::vector<std::string> split_string(const std::string& string_to_split, const std::string& pattern);

std::string trim_right(const std::string& s, const std::string& delimiters);

std::string trim_left(const std::string& s, const std::string& delimiters);

std::string trim(const std::string& s, const std::string& delimiters);

/// Returns the byte offset for the given character on the given line.
int find_position_offset(const char* text, int line, int character);

/// Returns `true` if the character may start an identifier.
bool is_identifier_start_char(char c);

/// Returns `true` if the character may be part of an identifier.
bool is_identifier_char(char c);

/// Returns the offset in `text` where the last word started.
int get_last_word_start(const char* text, int offset);

/// Given an index inside a word, returns the index of the end of the word (ie.
/// one past the last character)
int get_word_end(const char* text, int start);
