#include "symbols.hpp"
#include "utils.hpp"

#include <vector>

void add_builtin_types(SymbolMap& symbols)  {
    symbols.emplace("bool", Symbol{Symbol::Type, "<type>"});
    symbols.emplace("int", Symbol{Symbol::Type, "<type>"});
    symbols.emplace("uint", Symbol{Symbol::Type, "<type>"});
    symbols.emplace("float", Symbol{Symbol::Type, "<type>"});
    symbols.emplace("double", Symbol{Symbol::Type, "<type>"});

    std::string vec_buffer = "TvecX";
    for (int i = 2; i <= 4; i++) {
        vec_buffer[4] = '0' + i;

        // vec2, vec3, vec4, etc.
        symbols.emplace(&vec_buffer[1], Symbol{Symbol::Type, "<type>"});
        // bvecX, ivecX, uvecX, dvecX
        vec_buffer[0] = 'b';
        symbols.emplace(vec_buffer, Symbol{Symbol::Type, "<type>"});
        vec_buffer[0] = 'i';
        symbols.emplace(vec_buffer, Symbol{Symbol::Type, "<type>"});
        vec_buffer[0] = 'u';
        symbols.emplace(vec_buffer, Symbol{Symbol::Type, "<type>"});
        vec_buffer[0] = 'd';
        symbols.emplace(vec_buffer, Symbol{Symbol::Type, "<type>"});
    }

    std::string mat_buffer = "dmatXxX";
    for (int col = 2; col <= 4; col++) {
        mat_buffer[4] = '0' + col;

        for (int row = 2; row <= 4; row++) {
            mat_buffer[6] = '0' + row;
            symbols.emplace(&mat_buffer[1], Symbol{Symbol::Type, "<type>"});
            symbols.emplace(mat_buffer, Symbol{Symbol::Type, "<type>"});
        }

        mat_buffer[5] = 0;
        symbols.emplace(&mat_buffer[0], Symbol{Symbol::Type, "<type>"});
        symbols.emplace(&mat_buffer[1], Symbol{Symbol::Type, "<type>"});
        mat_buffer[5] = 'x';
    }
    
    const char* image_kinds[] = {
        "1D",
        "2D",
        "3D",
        "Cube",
        "2DRect",
        "1DArray",
        "2DArray",
        "CubeArray",
        "Buffer",
        "2DMS",
        "2DMSArray",
    };

    int image_count = sizeof(image_kinds) / sizeof(image_kinds[0]);
    for (int i = 0; i < image_count; i++) {
        std::string buffer = "gimage";
        buffer += image_kinds[i];

        symbols.emplace(&buffer[1], Symbol{Symbol::Type, "<type>"});
        buffer[0] = 'i';
        symbols.emplace(buffer, Symbol{Symbol::Type, "<type>"});
        buffer[0] = 'u';
        symbols.emplace(buffer, Symbol{Symbol::Type, "<type>"});
    }

    for (int i = 0; i < image_count; i++) {
        std::string buffer = "gsampler";
        buffer += image_kinds[i];

        symbols.emplace(&buffer[1], Symbol{Symbol::Type, "<type>"});
        buffer[0] = 'i';
        symbols.emplace(buffer, Symbol{Symbol::Type, "<type>"});
        buffer[0] = 'u';
        symbols.emplace(buffer, Symbol{Symbol::Type, "<type>"});
    }

    const char* shadow_samplers[] = {
        "sampler1DShadow",
        "sampler2DShadow",
        "samplerCubeShadow",
        "sampler2DRectShadow",
        "sampler1DArrayShadow",
        "sampler2DArrayShadow",
        "samplerCubeArrayShadow",
    };
    int shadow_sampler_count = sizeof(shadow_samplers) / sizeof(shadow_samplers[0]);
    for (int i = 0; i < shadow_sampler_count; i++) {
        symbols.emplace(shadow_samplers[i], Symbol{Symbol::Type, "<type>"});
    }
}

struct Word {
    const char* start = nullptr;
    const char* end = nullptr;

    bool is_equal(const char* text) const {
        const char* s = start;
        while (s != end && *s == *text) {
            s++;
            text++;
        }
        return s == end && *text == 0;
    }
};

bool is_whitespace(char c) {
    return c == ' ' || c == '\t' || c == '\n';
}

/// Extracts all global symbols from the given string, and inserts them into the symbol map.
/// This will not register symbols within function bodies, as they are context dependent.
///
/// The current implementation uses naive heuristics and thus may not handle
/// certain cases that well, and also give wrong results. This should be
/// replaced with an actual parser, but is workable for now.
void extract_symbols(const char* text, SymbolMap& symbols, const char* uri) {
    std::vector<Word> words;
    int arguments = 0;
    bool had_arguments = false;
    Word array{};
    Word inside_block{};

    const char* p = text;
    while (*p) {
        if (is_identifier_start_char(*p)) {
            const char* start = p;
            while (is_identifier_char(*p)) p++;
            Word ident{start, p};

            if (*p == '[') {
                const char* array_start = p;
                while (*p && *p != ']') p++;
                array = Word{array_start, *p == ']' ? p+1 : p};
            }

            // don't confuse `layout(...)` for a function.
            if (ident.is_equal("layout")) {
                while (is_whitespace(*p)) p++;
                if (*p == '(') {
                    while (*p && *p != ')') p++;
                }
                continue;
            }

            words.push_back(ident);
            continue;
        } 

        // don't confuse numeric literals as identifiers
        if ('0' <= *p && *p <= '9') {
            p++;
            while (is_identifier_char(*p)) p++;
            continue;
        } 

        if (*p == '{') {
            // TODO: handle function bodies

            if (words.size() >= 2 && arguments == 0) {
                Word kind = words[words.size() - 2];
                if (kind.is_equal("in") 
                        || kind.is_equal("out") 
                        || kind.is_equal("uniform") 
                        || kind.is_equal("buffer")) {
                    inside_block = words[words.size() - 1];
                    words.clear();
                    p++;
                    continue;
                }
            }

            // skip struct fields and function bodies (their contents are not global)
            while (*p && *p != '}') p++;
            continue;
        } 

        if (*p == '}' && inside_block.start) {
            words.push_back(inside_block);
            inside_block = Word{};
        }

        if (*p == '(') {
            had_arguments = true;
            p++;
            const char* start = nullptr;
            const char* end = nullptr;
            while (*p) {
                if (is_whitespace(*p)) {
                    p++;
                    continue;
                }

                if (*p == ')' || *p == ',') {
                    if (start) {
                        words.push_back({start, p});
                        arguments++;
                    }

                    if (*p == ')') break;

                    p++;
                    start = nullptr;
                    end = nullptr;
                    continue;
                }

                if (!start) start = p;
                end = p;

                p++;
            }
        } 

        if (*p == ';' || *p == ')' || *p == '=') {
            // end of declaration
            int name_index = (int)words.size() - arguments - 1;
            int type_index = name_index - 1;

            if (name_index >= 0) {
                Word name_word = words[name_index];
                Word type_word = type_index >= 0 ? words[type_index] : Word{};

                std::string name(name_word.start, name_word.end);
                std::string type(type_word.start, type_word.end);

                if (!type.empty()) {
                    if (symbols.find(type) == symbols.end()) {
                        symbols.emplace(type, Symbol{Symbol::Type, "<type>"});
                    }
                }

                if (arguments == 0 && array.start) {
                    type.append(array.start, array.end);
                }

                for (int i = 0; i < arguments; i++) {
                    if (i == 0) {
                        type += " (";
                    } else {
                        type += ", ";
                    }

                    Word arg = words[name_index + 1 + i];
                    const char* t = arg.start;
                    while (t != arg.end) {
                        if (is_whitespace(*t)) {
                            // only emit a single space
                            type.push_back(' ');
                            while (t != arg.end && is_whitespace(*t)) t++;
                        } else {
                            type.push_back(*t);
                            t++;
                        }
                    }

                    if (i == arguments - 1) {
                        type += ")";
                    }
                }

                Symbol::Kind kind = *p == ')' ? Symbol::Function : Symbol::Constant;
                int offset = name_word.start - text;
                symbols.emplace(name, Symbol{kind, type, {uri, offset}});
            }

            words.clear();
            arguments = 0;
            had_arguments = false;
            array = Word{};

            if (*p == '=') {
                // if we have a constant assignment, skip over the expression
                while (*p && *p != ';') p++;
            }
        }

        p++;
    }
}

