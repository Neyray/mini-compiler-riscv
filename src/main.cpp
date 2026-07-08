#include <iostream>
#include <string>
#include <sstream>
#include "Lexer.h"
#include "Parser.h"
#include "SemanticAnalyzer.h"
#include "CodeGen.h"

int main(int argc, char* argv[]) {
    bool optEnabled = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "-opt") {
            optEnabled = true;
        }
    }

    std::stringstream buffer;
    buffer << std::cin.rdbuf();
    std::string source = buffer.str();

    if (source.empty()) {
        std::cerr << "Error: empty input." << std::endl;
        return 1;
    }

    try {
        Lexer lexer(source);
        Parser parser(lexer);
        auto ast = parser.parseCompUnit();

        SemanticAnalyzer semantic;
        CheckedAST checked = semantic.check(ast.get());

        CodeGen codegen(optEnabled);
        codegen.generate(checked.root, std::cout);
    } catch (const SemanticError& e) {
        std::cerr << e.what() << std::endl;
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "Compilation failed: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
