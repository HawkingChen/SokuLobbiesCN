#ifndef SOKULOBBIES_HTTP_RESPONSE_BUFFER_HPP
#define SOKULOBBIES_HTTP_RESPONSE_BUFFER_HPP

#include <string>
#include <cstddef>

// A C callback must report failure to curl, never unwind through its C frames.
template<class Body>
inline size_t appendHttpResponse(Body &body, const void *data, size_t size,
    size_t count, size_t limit = 12 * 1024 * 1024) noexcept
{
    if (body.size() > limit || (size && count > (limit - body.size()) / size))
        return 0;
    const size_t bytes = size * count;
    if (!bytes) return 0;
    if (!data) return 0;
    try { body.append(static_cast<const char *>(data), bytes); return bytes; }
    catch (...) { return 0; }
}

#endif
