#pragma once

#include <string>
#include <string_view>

namespace bloodstained::connection {

inline std::string_view RemoveWebSocketScheme(std::string_view uri) {
    if (uri.starts_with("ws://")) return uri.substr(5);
    if (uri.starts_with("wss://")) return uri.substr(6);
    return uri;
}

inline std::string_view Host(std::string_view uri) {
    const std::string_view address = RemoveWebSocketScheme(uri);
    const std::string_view authority = address.substr(0, address.find('/'));
    if (authority.starts_with('[')) {
        const auto closingBracket = authority.find(']');
        return closingBracket == std::string_view::npos ? authority : authority.substr(1, closingBracket - 1);
    }
    return authority.substr(0, authority.find(':'));
}

inline bool IsLoopback(std::string_view uri) {
    const std::string_view host = Host(uri);
    return host == "localhost" || host == "127.0.0.1" || host == "::1";
}

inline std::string PrepareServerUri(std::string_view uri, std::string_view defaultUri) {
    if (uri.empty()) uri = defaultUri;
    if (IsLoopback(uri)) return "ws://" + std::string(RemoveWebSocketScheme(uri));
    return std::string(uri);
}

}  // namespace bloodstained::connection
