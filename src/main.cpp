#include "CLI/CLI.hpp"

#include "json.hpp"

#include "mongoose.h"

#include "ResourceLimits.h"
#include "ShaderLang.h"

#include <cstdint>
#include <experimental/filesystem>
#include <fstream>
#include <iostream>
#include <string>

using json = nlohmann::json;
namespace fs = std::experimental::filesystem;

const char* s_http_port = "8000";
struct mg_serve_http_opts s_http_server_opts;

std::string make_response(std::string& content)
{
    std::string header;
    header.append("Content-Length: " + std::to_string(content.length()) + "\r\n");
    header.append("Content-Type: application/vscode-jsonrpc;charset=utf-8\r\n");
    header.append("\r\n");
    return header + content;
}

void ev_handler(struct mg_connection* c, int ev, void* p)
{
    if (ev == MG_EV_HTTP_REQUEST) {
        struct http_message* hm = (struct http_message*)p;

        std::string content = hm->message.p;
        std::string response = make_response(content);
        mg_send_head(c, 200, response.length(), "Content-Type: text/plain");
        mg_printf(c, "%.*s", static_cast<int>(response.length()), response.c_str());
    }
}

EShLanguage find_language(const std::string& name)
{
    auto ext = fs::path(name).extension();
    if (ext == ".vert")
        return EShLangVertex;
    else if (ext == ".tesc")
        return EShLangTessControl;
    else if (ext == ".tese")
        return EShLangTessEvaluation;
    else if (ext == ".geom")
        return EShLangGeometry;
    else if (ext == ".frag")
        return EShLangFragment;
    else if (ext == ".comp")
        return EShLangCompute;
}

int main(int argc, char* argv[])
{
    CLI::App app{ "GLSL Language Server" };

    bool use_stdin = false;
    bool verbose = false;
    uint16_t port = 61313;
    std::string logfile;
    auto stdin_option = app.add_flag("--stdin", use_stdin, "Don't launch an HTTP server and instead accept input on stdin");
    app.add_flag("-v,--verbose", verbose, "Enable verbose logging");
    app.add_option("-l,--log", logfile, "Log file");
    app.add_option("-p,--port", port, "Port", true)->excludes(stdin_option);

    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError& e) {
        return app.exit(e);
    }

    std::ofstream logfile_stream;
    if (!logfile.empty()) {
        logfile_stream.open(logfile);
    }

    if (!use_stdin) {
        struct mg_mgr mgr;
        struct mg_connection* nc;

        glslang::InitializeProcess();
        auto document = "lol.vert";
        std::ifstream shader_file(document);
        std::string shader_string;
        shader_string.assign(
            (std::istreambuf_iterator<char>(shader_file)),
            (std::istreambuf_iterator<char>()));
        auto shader_cstring = shader_string.c_str();
        auto lang = find_language(document);
        glslang::TShader shader(lang);
        shader.setStrings(&shader_cstring, 1);
        TBuiltInResource Resources = glslang::DefaultTBuiltInResource;
        EShMessages messages = EShMsgDefault;
        shader.parse(&Resources, 110, false, messages);
        // std::cout << shader.getInfoLog() << std::endl;
        std::string debug_log = shader.getInfoLog();
        std::cout << debug_log << std::endl;
        glslang::FinalizeProcess();

        mg_mgr_init(&mgr, NULL);
        printf("Starting web server on port %s\n", s_http_port);
        nc = mg_bind(&mgr, s_http_port, ev_handler);
        if (nc == NULL) {
            printf("Failed to create listener\n");
            return 1;
        }

        // Set up HTTP server parameters
        mg_set_protocol_http_websocket(nc);

        while (true) {
            mg_mgr_poll(&mgr, 1000);
        }
        mg_mgr_free(&mgr);
    } else {
        std::string lols;
        while (!(std::cin >> lols).eof()) {
            if (!logfile.empty()) {
                logfile_stream << lols << std::endl;
            }
        }
    }

    if (!logfile.empty()) {
        logfile_stream.close();
    }

    return 0;
}
