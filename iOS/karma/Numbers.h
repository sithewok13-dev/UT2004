#pragma once

// Takes the place of metoolkit-karma/src/internal/Numbers.h in the iOS build, see the top CMakeLists.txt.
// Apple's libc++ has floating point from_chars only from iOS 26 and to_chars from iOS 16.3, so the
// floating point numbers go through the C library with an explicit "C" locale. Same interface as the original.

#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <type_traits>
#include <xlocale.h>

inline locale_t karmaCLocale()
{
    static locale_t loc = newlocale(LC_ALL_MASK, "C", nullptr);
    return loc;
}

// strtod semantics: leading space and a '+' are skipped, and *end == s means nothing was read. Unlike strtol,
// an out of range value or a '-' on an unsigned type is not read either.
template <class T>
T parseNumber(const char* s, char** end)
{
    const char* p = s + std::strspn(s, " \t\r\n");
    if (*p == '+' && p[1] != '-')
        ++p;
    T v{};
    const char* e;
    if constexpr (std::is_floating_point_v<T>)
    {
        char* d;
        v = (T)strtod_l(p, &d, karmaCLocale());
        e = d != p ? d : nullptr;
    }
    else
    {
        auto r = std::from_chars(p, p + std::strlen(p), v);
        e = r.ec == std::errc() ? r.ptr : nullptr;
    }
    bool ok = e != nullptr;
    // strtod accepts "nan" and "inf", which no Karma file holds and which would poison the simulation.
    if constexpr (std::is_floating_point_v<T>)
        ok = ok && std::isfinite(v);
    *end = const_cast<char*>(ok ? e : s);
    return ok ? v : T{};
}

// The shortest text that reads back to the same value.
template <class T>
std::string formatReal(T v)
{
    char buf[32];
    if constexpr (std::is_floating_point_v<T>)
    {
        const int digits = std::is_same_v<T, float> ? 9 : 17;
        for (int prec = 1; prec <= digits; prec++)
        {
            snprintf_l(buf, sizeof(buf), karmaCLocale(), "%.*g", prec, (double)v);
            if ((T)strtod_l(buf, nullptr, karmaCLocale()) == v)
                break;
        }
        return buf;
    }
    else
        return std::string(buf, std::to_chars(buf, buf + sizeof(buf), v).ptr);
}
