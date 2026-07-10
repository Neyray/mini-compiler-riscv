#include <iostream>
#include <string>
#include <sstream>
#include "Lexer.h"
#include "Parser.h"
#include "SemanticAnalyzer.h"
#include "Optimizer.h"
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

        if (optEnabled) {
            // AST 级优化：全局常量传播、常量/复写传播、常量折叠、代数化简、
            // 公共子表达式消除、死代码删除、循环不变量外提。
            Optimizer optimizer;
            optimizer.run(checked.root);
        }

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
