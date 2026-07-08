#include "SymbolTable.h"

void SymbolTable::enterScope() {
    scopes.push_back({});
}

void SymbolTable::exitScope() {
    if (scopes.empty()) {
        throw std::runtime_error("internal error: exit empty scope");
    }
    scopes.pop_back();
}

bool SymbolTable::isGlobalScope() const {
    return scopes.size() == 1;
}

int SymbolTable::depth() const {
    return static_cast<int>(scopes.size());
}

bool SymbolTable::currentScopeContains(const std::string& name) const {
    if (scopes.empty()) {
        return false;
    }
    return scopes.back().find(name) != scopes.back().end();
}

bool SymbolTable::declare(const Symbol& sym) {
    if (scopes.empty()) {
        enterScope();
    }

    auto& cur = scopes.back();
    if (cur.find(sym.name) != cur.end()) {
        return false;
    }

    cur[sym.name] = sym;
    return true;
}

Symbol* SymbolTable::lookup(const std::string& name) {
    for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
        auto found = it->find(name);
        if (found != it->end()) {
            return &found->second;
        }
    }
    return nullptr;
}

const Symbol* SymbolTable::lookup(const std::string& name) const {
    for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
        auto found = it->find(name);
        if (found != it->end()) {
            return &found->second;
        }
    }
    return nullptr;
}
