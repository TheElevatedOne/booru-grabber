#include "core/datetime.hpp"

#include <cstring>

#include "core/util.hpp"

namespace bg {

DateTime::DateTime(GDateTime* dt) {
    if(dt) m_dt = std::shared_ptr<GDateTime>(dt, g_date_time_unref);
}

DateTime DateTime::now() { return DateTime(g_date_time_new_now_local()); }
DateTime DateTime::nowUtc() { return DateTime(g_date_time_new_now_utc()); }

DateTime DateTime::fromUnix(int64_t secs, bool utc) {
    return DateTime(utc ? g_date_time_new_from_unix_utc(secs) : g_date_time_new_from_unix_local(secs));
}

DateTime DateTime::fromUnixMs(int64_t ms, bool utc) {
    GDateTime* base = utc ? g_date_time_new_from_unix_utc(ms / 1000) : g_date_time_new_from_unix_local(ms / 1000);
    if(!base) return {};
    GDateTime* r = g_date_time_add(base, (ms % 1000) * 1000);
    g_date_time_unref(base);
    return DateTime(r);
}

DateTime DateTime::fromIso(const std::string& in, bool assumeUtc) {
    std::string s = trim(in);
    if(s.size() < 10) return {};
    if(s.size() == 10) s += "T00:00:00";
    if(s.size() > 10 && (s[10] == ' ' || s[10] == 't')) s[10] = 'T';
    GTimeZone* tz = assumeUtc ? g_time_zone_new_utc() : g_time_zone_new_local();
    GDateTime* dt = g_date_time_new_from_iso8601(s.c_str(), tz);
    if(!dt) {
        // GLib requires ":" in offsets; accept "+0000" too
        static const Regex rx("([+-]\\d{2})(\\d{2})$");
        std::string fixed = rx.replace(s, "\\1:\\2");
        dt = g_date_time_new_from_iso8601(fixed.c_str(), tz);
    }
    g_time_zone_unref(tz);
    return DateTime(dt);
}

static int monthIndex(const std::string& m) {
    static const char* months[] = {"jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sep", "oct", "nov", "dec"};
    std::string l = toLower(m.substr(0, 3));
    for(int i = 0; i < 12; i++)
        if(l == months[i]) return i + 1;
    return 0;
}

static DateTime makeDate(int y, int mo, int d, int h, int mi, double s, GTimeZone* tz) {
    if(y <= 0 || mo < 1 || mo > 12 || d < 1 || d > 31) return {};
    return DateTime(g_date_time_new(tz, y, mo, d, h, mi, s));
}

DateTime DateTime::parse(const std::string& in) {
    std::string str = trim(in);
    if(str.empty()) return {};

    // UTC timestamp
    unsigned long long ts = toUInt(str, 0);
    if(ts != 0) return fromUnix((int64_t)ts, true);

    // ISO 8601 (without offset means UTC)
    if(str.size() >= 16 && str[4] == '-' && (str[10] == 'T' || str[10] == ' ')) return fromIso(str, true);
    if(str.size() == 10 && str[4] == '-' && str[7] == '-') return fromIso(str, true);

    GTimeZone* utc = g_time_zone_new_utc();
    DateTime ret;
    // yyyy/MM/dd HH:mm[:ss[.zzz]]
    if(str.size() >= 16 && str[4] == '/' && str[7] == '/') {
        int y = (int)toInt(str.substr(0, 4)), mo = (int)toInt(str.substr(5, 2)), d = (int)toInt(str.substr(8, 2));
        int h = (int)toInt(str.substr(11, 2)), mi = (int)toInt(str.substr(14, 2));
        double s = str.size() >= 19 ? toDouble(str.substr(17, 2)) : 0;
        if(str.size() >= 23 && str[19] == '.') s += toDouble("0" + str.substr(19, 4));
        ret = makeDate(y, mo, d, h, mi, s, utc);
    } else {
        // "Sat May 14 17:38:04 2011" / "Sat May  4 17:38:04 2011" / "Sat May 14 17:38:04 -0400 2011"
        StringList parts = split(str, ' ', true);
        if(parts.size() >= 5 && monthIndex(parts[1]) > 0) {
            int mo = monthIndex(parts[1]);
            int d = (int)toInt(parts[2]);
            StringList t = split(parts[3], ':');
            int h = t.size() > 0 ? (int)toInt(t[0]) : 0, mi = t.size() > 1 ? (int)toInt(t[1]) : 0;
            double s = t.size() > 2 ? toDouble(t[2]) : 0;
            if(parts.size() >= 6 && (parts[4][0] == '+' || parts[4][0] == '-') && parts[4].size() == 5) {
                int y = (int)toInt(parts[5]);
                std::string off = parts[4].substr(0, 3) + ":" + parts[4].substr(3, 2);
                GTimeZone* tz = g_time_zone_new_identifier(off.c_str());
                if(tz) {
                    ret = makeDate(y, mo, d, h, mi, s, tz);
                    g_time_zone_unref(tz);
                }
            } else {
                int y = (int)toInt(parts[4]);
                ret = makeDate(y, mo, d, h, mi, s, utc);
            }
        }
    }
    g_time_zone_unref(utc);
    return ret;
}

DateTime DateTime::parseDate(const std::string& s) {
    GTimeZone* local = g_time_zone_new_local();
    DateTime r;
    if(s.size() == 10 && s[4] == '-')
        r = makeDate((int)toInt(s.substr(0, 4)), (int)toInt(s.substr(5, 2)), (int)toInt(s.substr(8, 2)), 0, 0, 0, local);
    else if(s.size() == 10 && s[2] == '/')
        r = makeDate((int)toInt(s.substr(6, 4)), (int)toInt(s.substr(0, 2)), (int)toInt(s.substr(3, 2)), 0, 0, 0, local);
    g_time_zone_unref(local);
    return r;
}

int64_t DateTime::toUnix() const { return m_dt ? g_date_time_to_unix(m_dt.get()) : 0; }
int64_t DateTime::toUnixMs() const {
    return m_dt ? g_date_time_to_unix(m_dt.get()) * 1000 + g_date_time_get_microsecond(m_dt.get()) / 1000 : 0;
}

std::string DateTime::toIso() const {
    if(!m_dt) return "";
    GTimeSpan off = g_date_time_get_utc_offset(m_dt.get());
    gchar* s = g_date_time_format(m_dt.get(), off == 0 ? "%Y-%m-%dT%H:%M:%SZ" : "%Y-%m-%dT%H:%M:%S%:z");
    std::string r = s ? s : "";
    g_free(s);
    return r;
}

DateTime DateTime::toLocal() const { return m_dt ? DateTime(g_date_time_to_local(m_dt.get())) : DateTime(); }
DateTime DateTime::toUtc() const { return m_dt ? DateTime(g_date_time_to_utc(m_dt.get())) : DateTime(); }

DateTime DateTime::toTimeZone(const std::string& name) const {
    if(!m_dt) return {};
    GTimeZone* tz = g_time_zone_new_identifier(name.c_str());
    if(!tz) return *this;
    DateTime r(g_date_time_to_timezone(m_dt.get(), tz));
    g_time_zone_unref(tz);
    return r;
}

DateTime DateTime::addSecs(int64_t s) const { return m_dt ? DateTime(g_date_time_add_seconds(m_dt.get(), (gdouble)s)) : DateTime(); }
DateTime DateTime::addDays(int d) const { return m_dt ? DateTime(g_date_time_add_days(m_dt.get(), d)) : DateTime(); }
DateTime DateTime::addMonths(int m) const { return m_dt ? DateTime(g_date_time_add_months(m_dt.get(), m)) : DateTime(); }
DateTime DateTime::addYears(int y) const { return m_dt ? DateTime(g_date_time_add_years(m_dt.get(), y)) : DateTime(); }
int64_t DateTime::secsTo(const DateTime& o) const {
    if(!m_dt || !o.m_dt) return 0;
    return g_date_time_difference(o.m_dt.get(), m_dt.get()) / G_TIME_SPAN_SECOND;
}

bool DateTime::operator<(const DateTime& o) const {
    if(!m_dt || !o.m_dt) return !m_dt && o.m_dt;
    return g_date_time_compare(m_dt.get(), o.m_dt.get()) < 0;
}

bool DateTime::operator==(const DateTime& o) const {
    if(!m_dt || !o.m_dt) return !m_dt && !o.m_dt;
    return g_date_time_equal(m_dt.get(), o.m_dt.get());
}

std::string DateTime::toDisplayString() const { return valid() ? toLocal().format("yyyy-MM-dd HH:mm") : ""; }

// Qt date format: d dd ddd dddd M MM MMM MMMM yy yyyy h hh H HH m mm s ss z zzz AP ap t, '...' literals.
std::string DateTime::format(const std::string& f) const {
    if(!m_dt) return "";
    static const char* days[] = {"Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday"};
    static const char* months[] = {"January", "February", "March", "April", "May", "June",
                                   "July", "August", "September", "October", "November", "December"};
    GDateTime* dt = m_dt.get();
    bool ampm = f.find("AP") != std::string::npos || f.find("ap") != std::string::npos ||
                f.find("A") != std::string::npos || f.find("a") != std::string::npos;
    // "a"/"A" only count when not part of a quoted literal; keep it simple and check quotes below.
    {
        bool q = false;
        ampm = false;
        for(char c : f) {
            if(c == '\'') q = !q;
            else if(!q && (c == 'a' || c == 'A')) ampm = true;
        }
    }
    std::string out;
    size_t i = 0;
    auto two = [](int v) { return v < 10 ? "0" + std::to_string(v) : std::to_string(v); };
    while(i < f.size()) {
        char c = f[i];
        if(c == '\'') {
            size_t j = i + 1;
            if(j < f.size() && f[j] == '\'') {
                out += '\'';
                i += 2;
                continue;
            }
            while(j < f.size()) {
                if(f[j] == '\'') {
                    if(j + 1 < f.size() && f[j + 1] == '\'') {
                        out += '\'';
                        j += 2;
                        continue;
                    }
                    break;
                }
                out += f[j++];
            }
            i = j + 1;
            continue;
        }
        size_t n = 1;
        while(i + n < f.size() && f[i + n] == c) n++;
        int hour = g_date_time_get_hour(dt);
        switch(c) {
            case 'd': {
                int dd = g_date_time_get_day_of_month(dt);
                int wd = g_date_time_get_day_of_week(dt) - 1;
                if(n == 1) out += std::to_string(dd);
                else if(n == 2) out += two(dd);
                else if(n == 3) out += std::string(days[wd]).substr(0, 3);
                else out += days[wd];
                n = std::min<size_t>(n, 4);
                break;
            }
            case 'M': {
                int mo = g_date_time_get_month(dt);
                if(n == 1) out += std::to_string(mo);
                else if(n == 2) out += two(mo);
                else if(n == 3) out += std::string(months[mo - 1]).substr(0, 3);
                else out += months[mo - 1];
                n = std::min<size_t>(n, 4);
                break;
            }
            case 'y': {
                int y = g_date_time_get_year(dt);
                if(n >= 4) {
                    out += std::to_string(y);
                    n = 4;
                } else if(n >= 2) {
                    out += two(y % 100);
                    n = 2;
                } else
                    out += c;
                break;
            }
            case 'h':
            case 'H': {
                int h = hour;
                if(c == 'h' && ampm) h = h % 12 == 0 ? 12 : h % 12;
                n = std::min<size_t>(n, 2);
                out += n == 2 ? two(h) : std::to_string(h);
                break;
            }
            case 'm': {
                int m = g_date_time_get_minute(dt);
                n = std::min<size_t>(n, 2);
                out += n == 2 ? two(m) : std::to_string(m);
                break;
            }
            case 's': {
                int s = g_date_time_get_second(dt);
                n = std::min<size_t>(n, 2);
                out += n == 2 ? two(s) : std::to_string(s);
                break;
            }
            case 'z': {
                int ms = g_date_time_get_microsecond(dt) / 1000;
                if(n >= 3) {
                    char b[16];
                    snprintf(b, sizeof b, "%03d", ms);
                    out += b;
                    n = 3;
                } else {
                    out += std::to_string(ms);
                    n = 1;
                }
                break;
            }
            case 'A':
            case 'a': {
                bool upper = c == 'A';
                out += hour < 12 ? (upper ? "AM" : "am") : (upper ? "PM" : "pm");
                n = (i + 1 < f.size() && (f[i + 1] == 'P' || f[i + 1] == 'p')) ? 2 : 1;
                break;
            }
            case 't': {
                gchar* tz = g_date_time_format(dt, "%Z");
                out += tz ? tz : "";
                g_free(tz);
                n = 1;
                break;
            }
            default:
                out.append(n, c);
        }
        i += n;
    }
    return out;
}

}  // namespace bg
