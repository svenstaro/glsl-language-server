#pragma once

#include <string>
#include <map>

struct Symbol {
    enum Kind {
        Unknown = 0,
        Function = 3,
        Type = 7,
        Constant = 21,
    };

    Kind kind = Unknown;
    std::string details;
};

typedef std::map<std::string, Symbol> SymbolMap;

/// Add the builtin types to the symbol map.
void add_builtin_types(SymbolMap& symbols);

/// Extracts symbols from the given file.
void extract_symbols(const char* text, SymbolMap& symbols);

