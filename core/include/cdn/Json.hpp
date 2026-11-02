// Minimal streaming JSON writer (no external dependencies).
#pragma once

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace cdn {

class JsonWriter {
public:
    JsonWriter& beginObject() { return open('{'); }
    JsonWriter& endObject() { return close('}'); }
    JsonWriter& beginArray() { return open('['); }
    JsonWriter& endArray() { return close(']'); }

    JsonWriter& key(const std::string& k) {
        comma();
        str(k);
        out_ += ':';
        afterKey_ = true;
        return *this;
    }
    JsonWriter& value(const std::string& s) {
        comma();
        str(s);
        return *this;
    }
    JsonWriter& value(const char* s) { return value(std::string(s)); }
    JsonWriter& value(bool b) {
        comma();
        out_ += b ? "true" : "false";
        return *this;
    }
    JsonWriter& value(double d) {
        comma();
        if (!std::isfinite(d)) {
            out_ += "null";
        } else {
            char buf[32];
            std::snprintf(buf, sizeof buf, "%.6g", d);
            out_ += buf;
        }
        return *this;
    }
    JsonWriter& value(long long v) {
        comma();
        out_ += std::to_string(v);
        return *this;
    }
    JsonWriter& value(long v) { return value(static_cast<long long>(v)); }
    JsonWriter& value(int v) { return value(static_cast<long long>(v)); }
    JsonWriter& value(unsigned long v) { return value(static_cast<long long>(v)); }
    JsonWriter& value(unsigned long long v) { return value(static_cast<long long>(v)); }

    template <class T>
    JsonWriter& kv(const std::string& k, const T& v) {
        key(k);
        return value(v);
    }
    template <class T>
    JsonWriter& array(const std::string& k, const std::vector<T>& v) {
        key(k);
        beginArray();
        for (const auto& x : v) value(x);
        return endArray();
    }

    const std::string& str() const { return out_; }

private:
    JsonWriter& open(char c) {
        comma();
        out_ += c;
        first_.push_back(true);
        return *this;
    }
    JsonWriter& close(char c) {
        out_ += c;
        first_.pop_back();
        return *this;
    }
    void comma() {
        if (afterKey_) {
            afterKey_ = false;
            return;
        }
        if (!first_.empty()) {
            if (!first_.back()) out_ += ',';
            first_.back() = false;
        }
    }
    void str(const std::string& s) {
        out_ += '"';
        for (char ch : s) {
            switch (ch) {
                case '"': out_ += "\\\""; break;
                case '\\': out_ += "\\\\"; break;
                case '\n': out_ += "\\n"; break;
                case '\t': out_ += "\\t"; break;
                case '\r': out_ += "\\r"; break;
                default:
                    if (static_cast<unsigned char>(ch) < 0x20) {
                        char buf[8];
                        std::snprintf(buf, sizeof buf, "\\u%04x", ch);
                        out_ += buf;
                    } else {
                        out_ += ch;
                    }
            }
        }
        out_ += '"';
    }

    std::string out_;
    std::vector<bool> first_;
    bool afterKey_ = false;
};

}  // namespace cdn
