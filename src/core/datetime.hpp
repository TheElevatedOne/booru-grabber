// Date/time value backed by GDateTime, with Qt-compatible format strings so filename
// formats written for the original Grabber ("%date:format=yyyy-MM-dd%") keep working.
#pragma once

#include <glib.h>

#include <memory>
#include <string>

namespace bg {

class DateTime {
public:
    DateTime() = default;
    explicit DateTime(GDateTime* dt);  // takes ownership

    static DateTime now();
    static DateTime nowUtc();
    static DateTime fromUnix(int64_t secs, bool utc = true);
    static DateTime fromUnixMs(int64_t ms, bool utc = true);
    // ISO 8601. Strings without an offset are interpreted in UTC (assumeUtc) or local time.
    static DateTime fromIso(const std::string& s, bool assumeUtc = true);
    // Lenient parser for the many date formats returned by boorus (see qDateTimeFromString).
    static DateTime parse(const std::string& s);
    // Parses "yyyy-MM-dd" or "MM/dd/yyyy" style dates (local midnight).
    static DateTime parseDate(const std::string& s);

    bool valid() const { return (bool)m_dt; }
    explicit operator bool() const { return valid(); }
    int64_t toUnix() const;
    int64_t toUnixMs() const;
    std::string toIso() const;  // e.g. 2024-01-31T10:20:30Z / +01:00
    DateTime toLocal() const;
    DateTime toUtc() const;
    DateTime toTimeZone(const std::string& tz) const;  // IANA name or "+01:00"
    DateTime addSecs(int64_t s) const;
    DateTime addDays(int d) const;
    DateTime addMonths(int m) const;
    DateTime addYears(int y) const;
    int64_t secsTo(const DateTime& other) const;
    std::string format(const std::string& qtFormat) const;
    std::string toDisplayString() const;  // local, "yyyy-MM-dd HH:mm"
    GDateTime* gdt() const { return m_dt.get(); }

    bool operator<(const DateTime& o) const;
    bool operator>(const DateTime& o) const { return o < *this; }
    bool operator<=(const DateTime& o) const { return !(o < *this); }
    bool operator>=(const DateTime& o) const { return !(*this < o); }
    bool operator==(const DateTime& o) const;
    bool operator!=(const DateTime& o) const { return !(*this == o); }

private:
    std::shared_ptr<GDateTime> m_dt;
};

}  // namespace bg
