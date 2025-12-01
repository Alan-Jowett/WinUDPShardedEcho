// Simple argument parser
#pragma once

#include <string>
#include <unordered_map>
#include <stdexcept>
#include <cstring>
#include <vector>
#include <iostream>

class ArgParser {
public:
    // Add an option: long name (e.g. "server"), short name (e.g. 's'), default value, and whether it takes a value
    void add_option(const std::string& long_name, char short_name, const std::string& default_value, bool takes_value = true, const std::string& description = "") {
        Option opt;
        opt.short_name = short_name;
        opt.default_value = default_value;
        opt.value = default_value;
        opt.takes_value = takes_value;
        opt.set = false;
        opt.description = description;
        opts_.emplace(long_name, std::move(opt));
        if (short_name != '\0') short_to_long_.emplace(short_name, long_name);
    }

    // Parse argc/argv; unknown options are ignored
    void parse(int argc, char* argv[]) {
        for (int i = 1; i < argc; ++i) {
            const char* a = argv[i];
            if (std::strncmp(a, "--", 2) == 0) {
                const char* name = a + 2;
                const char* eq = std::strchr(name, '=');
                std::string key;
                std::string val;
                if (eq) {
                    key.assign(name, eq - name);
                    val = std::string(eq + 1);
                } else {
                    key = name;
                }
                auto it = opts_.find(key);
                if (it == opts_.end()) continue;
                if (it->second.takes_value) {
                    if (!val.empty()) {
                        it->second.value = val;
                        it->second.set = true;
                    } else if (i + 1 < argc) {
                        it->second.value = argv[++i];
                        it->second.set = true;
                    }
                } else {
                    // flag
                    it->second.value = "1";
                    it->second.set = true;
                }
            } else if (a[0] == '-' && a[1] != '\0') {
                // short flags (only single-character supported)
                char shortname = a[1];
                auto itmap = short_to_long_.find(shortname);
                if (itmap == short_to_long_.end()) continue;
                auto it = opts_.find(itmap->second);
                if (it == opts_.end()) continue;
                if (it->second.takes_value) {
                    if (i + 1 < argc) {
                        it->second.value = argv[++i];
                        it->second.set = true;
                    }
                } else {
                    it->second.value = "1";
                    it->second.set = true;
                }
            } else {
                // Positional arguments are ignored by the parser
            }
        }
    }

    // Get the option value (parsed or default). Throws if option not registered.
    std::string get(const std::string& long_name) const {
        auto it = opts_.find(long_name);
        if (it == opts_.end()) throw std::invalid_argument("Unknown option: " + long_name);
        return it->second.value;
    }

    // Print help/usage based on registered options
    void print_help(const char* program_name) const {
        std::cout << "Usage: " << program_name << " [options]\n";
        std::cout << "Options:\n";
        for (const auto& kv : opts_) {
            const std::string& long_name = kv.first;
            const Option& opt = kv.second;
            std::cout << "  --" << long_name;
            if (opt.short_name != '\0') std::cout << ", -" << opt.short_name;
            if (opt.takes_value) std::cout << " <value>";
            std::cout << "\tDefault: " << opt.default_value;
            if (!opt.description.empty()) std::cout << "\t" << opt.description;
            std::cout << "\n";
        }
    }

    // Returns true if option was provided on command-line (not just default)
    bool is_set(const std::string& long_name) const {
        auto it = opts_.find(long_name);
        if (it == opts_.end()) return false;
        return it->second.set;
    }

private:
    struct Option {
        char short_name;
        std::string default_value;
        std::string value;
        bool takes_value;
        bool set;
        std::string description;
    };

    std::unordered_map<std::string, Option> opts_;
    std::unordered_map<char, std::string> short_to_long_;
};
