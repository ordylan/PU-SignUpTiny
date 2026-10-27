#pragma once

#include <string>
#include <vector>

namespace pu {

struct HttpResponse {
    long status = 0;
    std::string body;
};

class HttpSession {
public:
    HttpResponse get(const std::string& url,
                     const std::vector<std::string>& headers,
                     long timeout_seconds = 10) const;
    HttpResponse post(const std::string& url,
                      const std::vector<std::string>& headers,
                      const std::string& body,
                      long timeout_seconds = 10) const;
};

} // namespace pu
