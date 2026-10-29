// php2json — транслятор программ на PHP в JSON-описание по схеме из задания.
//   php2json [--short-types] [-o out.json] [input.php]

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "lexer.hpp"
#include "parser.hpp"

static void usage() {
    std::cerr << "Usage: php2json [options] [input.php]\n"
                 "  -o FILE         write JSON to FILE (default: standard output)\n"
                 "  --short-types   print types as plain strings (\"integer\") instead of type nodes\n"
                 "  -h, --help      show this help\n"
                 "Reads standard input if no input file is given.\n";
}

int main(int argc, char** argv) {
    std::string inPath, outPath;
    Options opt;
    for (int k = 1; k < argc; ++k) {
        std::string a = argv[k];
        if (a == "-h" || a == "--help") {
            usage();
            return 0;
        } else if (a == "-o") {
            if (++k >= argc) {
                usage();
                return 2;
            }
            outPath = argv[k];
        } else if (a == "--short-types") {
            opt.shortTypes = true;
        } else if (a.size() > 1 && a[0] == '-') {
            std::cerr << "php2json: unknown option " << a << "\n";
            usage();
            return 2;
        } else {
            inPath = a;
        }
    }

    std::string src;
    {
        std::ostringstream ss;
        if (inPath.empty() || inPath == "-") {
            ss << std::cin.rdbuf();
        } else {
            std::ifstream f(inPath, std::ios::binary);
            if (!f) {
                std::cerr << "php2json: cannot open " << inPath << "\n";
                return 1;
            }
            ss << f.rdbuf();
        }
        src = ss.str();
    }
    if (src.size() >= 3 && static_cast<unsigned char>(src[0]) == 0xEF && static_cast<unsigned char>(src[1]) == 0xBB &&
        static_cast<unsigned char>(src[2]) == 0xBF)
        src.erase(0, 3);  // UTF-8 BOM

    try {
        Lexer lexer(src);
        Parser parser(lexer.tokenize(), opt);
        std::string json = parser.parseProgram().dump(2);
        if (outPath.empty()) {
            std::cout << json;
        } else {
            std::ofstream o(outPath, std::ios::binary);
            if (!o) {
                std::cerr << "php2json: cannot write " << outPath << "\n";
                return 1;
            }
            o << json;
        }
    } catch (const std::exception& e) {
        std::cerr << "php2json: " << (inPath.empty() ? "<stdin>" : inPath) << ": " << e.what() << "\n";
        return 1;
    }
    return 0;
}
