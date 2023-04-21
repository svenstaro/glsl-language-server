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

    struct Location {
        /// Name of the file the symbol is defined in. If `null` this is undefined.
        const char* uri = nullptr;
        /// If the uri is not `null`, the offset into the file where the symbol is defined.
        int offset = -1;
    } location;
};

typedef std::map<std::string, Symbol> SymbolMap;

/// Add the builtin types to the symbol map.
void add_builtin_types(SymbolMap& symbols);

/// Extracts symbols from the given file.
void extract_symbols(const char* text, SymbolMap& symbols, const char* uri = nullptr);

