#ifndef TOML_PARSER_H
#define TOML_PARSER_H

#include <string>
#include <map>
#include <fstream>
#include <sstream>
#include <iostream>

// Simple TOML parser for calibration files
// Supports sections [name] and key = value pairs (strings, floats, ints)
class TomlParser {
public:
    std::map<std::string, std::map<std::string, std::string>> sections;
    
    bool parse(const std::string& filename) {
        std::ifstream file(filename);
        if (!file.is_open()) {
            std::cerr << "Failed to open TOML file: " << filename << std::endl;
            return false;
        }
        
        std::string currentSection = "";
        std::string line;
        int lineNum = 0;
        
        while (std::getline(file, line)) {
            lineNum++;
            
            // Trim whitespace
            size_t start = line.find_first_not_of(" \t\r\n");
            if (start == std::string::npos) continue;
            line = line.substr(start);
            
            size_t end = line.find_last_not_of(" \t\r\n");
            if (end != std::string::npos) {
                line = line.substr(0, end + 1);
            }
            
            // Skip empty lines and comments
            if (line.empty() || line[0] == '#') continue;
            
            // Section header
            if (line[0] == '[' && line.back() == ']') {
                currentSection = line.substr(1, line.size() - 2);
                sections[currentSection] = {};
                continue;
            }
            
            // Key-value pair
            size_t eqPos = line.find('=');
            if (eqPos != std::string::npos) {
                std::string key = line.substr(0, eqPos);
                std::string value = line.substr(eqPos + 1);
                
                // Trim key
                end = key.find_last_not_of(" \t");
                if (end != std::string::npos) key = key.substr(0, end + 1);
                
                // Trim value
                start = value.find_first_not_of(" \t");
                if (start != std::string::npos) value = value.substr(start);
                end = value.find_last_not_of(" \t\r\n");
                if (end != std::string::npos) value = value.substr(0, end + 1);
                
                // Remove quotes from strings
                if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
                    value = value.substr(1, value.size() - 2);
                }
                
                sections[currentSection][key] = value;
            }
        }
        
        return true;
    }
    
    float getFloat(const std::string& section, const std::string& key, float defaultValue = 0.0f) const {
        auto secIt = sections.find(section);
        if (secIt == sections.end()) return defaultValue;
        
        auto keyIt = secIt->second.find(key);
        if (keyIt == secIt->second.end()) return defaultValue;
        
        try {
            return std::stof(keyIt->second);
        } catch (...) {
            return defaultValue;
        }
    }
    
    int getInt(const std::string& section, const std::string& key, int defaultValue = 0) const {
        auto secIt = sections.find(section);
        if (secIt == sections.end()) return defaultValue;
        
        auto keyIt = secIt->second.find(key);
        if (keyIt == secIt->second.end()) return defaultValue;
        
        try {
            return std::stoi(keyIt->second);
        } catch (...) {
            return defaultValue;
        }
    }
    
    std::string getString(const std::string& section, const std::string& key, const std::string& defaultValue = "") const {
        auto secIt = sections.find(section);
        if (secIt == sections.end()) return defaultValue;
        
        auto keyIt = secIt->second.find(key);
        if (keyIt == secIt->second.end()) return defaultValue;
        
        return keyIt->second;
    }
    
    bool hasSection(const std::string& section) const {
        return sections.find(section) != sections.end();
    }
    
    bool hasKey(const std::string& section, const std::string& key) const {
        auto secIt = sections.find(section);
        if (secIt == sections.end()) return false;
        return secIt->second.find(key) != secIt->second.end();
    }
};

#endif // TOML_PARSER_H
