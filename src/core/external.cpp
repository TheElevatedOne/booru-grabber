#include "core/external.hpp"

#include <gio/gio.h>

#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>

#include <glib/gstdio.h>

#include "core/logger.hpp"

namespace bg::external {

ProcessResult run(const StringList& argv, int timeoutMs) {
    ProcessResult r;
    if(argv.empty()) return r;
    std::vector<const gchar*> args;
    for(auto& a : argv) args.push_back(a.c_str());
    args.push_back(nullptr);
    GError* err = nullptr;
    GSubprocess* p = g_subprocess_newv(args.data(), (GSubprocessFlags)(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE), &err);
    if(!p) {
        r.err = err ? err->message : "cannot start process";
        if(err) g_error_free(err);
        return r;
    }
    r.started = true;
    GCancellable* cancel = g_cancellable_new();
    std::mutex m;
    std::condition_variable cv;
    bool done = false;
    gchar *out = nullptr, *errOut = nullptr;
    std::thread t([&] {
        GError* e = nullptr;
        g_subprocess_communicate_utf8(p, nullptr, cancel, &out, &errOut, &e);
        if(e) g_error_free(e);
        std::lock_guard<std::mutex> l(m);
        done = true;
        cv.notify_one();
    });
    {
        std::unique_lock<std::mutex> l(m);
        if(!cv.wait_for(l, std::chrono::milliseconds(timeoutMs > 0 ? timeoutMs : 30000), [&] { return done; })) {
            r.timedOut = true;
            g_subprocess_force_exit(p);
            g_cancellable_cancel(cancel);
        }
    }
    t.join();
    g_subprocess_wait(p, nullptr, nullptr);
    if(g_subprocess_get_if_exited(p)) r.exitCode = g_subprocess_get_exit_status(p);
    if(out) r.out = out;
    if(errOut) r.err = errOut;
    g_free(out);
    g_free(errOut);
    g_object_unref(cancel);
    g_object_unref(p);
    return r;
}

bool available(const std::string& program) {
    gchar* p = g_find_program_in_path(program.c_str());
    bool ok = p != nullptr;
    g_free(p);
    return ok;
}

bool exiftoolSetMetadata(const std::string& file, const std::vector<std::pair<std::string, std::string>>& metadata, bool clear,
                         bool keepColorProfile, Sidecar sidecar, bool sidecarNoExtension) {
    if(!available("exiftool")) {
        log("exiftool is not installed, metadata not written", LogLevel::Warning);
        return false;
    }
    StringList tags;
    for(auto& [k, v] : metadata) tags.push_back("-" + k + "=" + v);
    auto writeSidecar = [&] {
        std::string xmp = (sidecarNoExtension ? setExtension(file, "") : file) + ".xmp";
        StringList args{"exiftool", "-overwrite_original", "-q", "-charset", "filename=utf8"};
        args.insert(args.end(), tags.begin(), tags.end());
        args.push_back(xmp);
        auto r = run(args);
        if(!r.ok()) log("exiftool could not write sidecar " + xmp + ": " + trim(r.err), LogLevel::Error);
        return r.ok();
    };
    if(sidecar == Sidecar::Only) return writeSidecar();
    StringList args{"exiftool", "-overwrite_original", "-q", "-charset", "filename=utf8"};
    if(clear) {
        args.push_back("-all=");
        if(keepColorProfile) {
            args.push_back("-tagsfromfile");
            args.push_back("@");
            args.push_back("-icc_profile");
        }
    }
    args.insert(args.end(), tags.begin(), tags.end());
    args.push_back(file);
    auto r = run(args);
    bool ok = r.ok();
    if(!ok) log("exiftool could not write metadata to " + file + ": " + trim(r.err), LogLevel::Warning);
    if(sidecar == Sidecar::Both || (sidecar == Sidecar::OnError && !ok)) writeSidecar();
    return ok;
}

std::string ffmpegVideoCodec(const std::string& file) {
    auto r = run({"ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries", "stream=codec_name", "-of",
                  "default=noprint_wrappers=1:nokey=1", file});
    return r.ok() ? trim(r.out) : "";
}

static std::string ffmpegRun(const std::string& file, const std::string& ext, bool overwrite, bool deleteOriginal, int timeoutMs,
                             const StringList& extra) {
    std::string dest = setExtension(file, ext);
    if(dest == file) return file;
    if(fileExists(dest) && !overwrite) {
        log("Conversion target `" + dest + "` already exists", LogLevel::Warning);
        return file;
    }
    StringList args{"ffmpeg", "-y", "-loglevel", "error", "-i", file};
    args.insert(args.end(), extra.begin(), extra.end());
    args.push_back(dest);
    auto r = run(args, timeoutMs);
    if(!r.ok()) {
        log("ffmpeg conversion of `" + file + "` failed: " + trim(r.err), LogLevel::Error);
        removeFile(dest);
        return file;
    }
    if(deleteOriginal) removeFile(file);
    return dest;
}

std::string ffmpegRemux(const std::string& file, const std::string& ext, bool overwrite, bool deleteOriginal, int timeoutMs) {
    return ffmpegRun(file, ext, overwrite, deleteOriginal, timeoutMs, {"-c", "copy"});
}

std::string ffmpegConvert(const std::string& file, const std::string& ext, bool overwrite, bool deleteOriginal, int timeoutMs) {
    StringList extra;
    if(ext == "mp4") extra = {"-pix_fmt", "yuv420p", "-vf", "pad=ceil(iw/2)*2:ceil(ih/2)*2"};
    return ffmpegRun(file, ext, overwrite, deleteOriginal, timeoutMs, extra);
}

std::string ffmpegConvertUgoira(const std::string& file, const std::vector<std::pair<std::string, int>>& frames, const std::string& ext,
                                bool overwrite, bool deleteOriginal, int timeoutMs) {
    if(frames.empty()) {
        log("No ugoira frame information for `" + file + "`, it cannot be converted", LogLevel::Warning);
        return file;
    }
    std::string dest = setExtension(file, ext);
    if(fileExists(dest) && !overwrite) return file;
    gchar* tmp = g_dir_make_tmp("booru-grabber-ugoira-XXXXXX", nullptr);
    if(!tmp) return file;
    std::string dir = tmp;
    g_free(tmp);
    std::string result = file;
    if(unzip(file, dir)) {
        std::string list;
        for(auto& [name, delay] : frames) {
            list += "file '" + replaceAll(buildPath(dir, name), "'", "'\\''") + "'\n";
            list += "duration " + fromDouble(delay / 1000.0, 3) + "\n";
        }
        list += "file '" + replaceAll(buildPath(dir, frames.back().first), "'", "'\\''") + "'\n";
        std::string listFile = buildPath(dir, "frames.txt");
        writeFile(listFile, list);
        StringList args{"ffmpeg", "-y", "-loglevel", "error", "-f", "concat", "-safe", "0", "-i", listFile};
        if(ext == "gif") {
            args.insert(args.end(), {"-vf", "split[s0][s1];[s0]palettegen[p];[s1][p]paletteuse", "-loop", "0"});
        } else if(ext == "mp4") {
            args.insert(args.end(), {"-pix_fmt", "yuv420p", "-vf", "pad=ceil(iw/2)*2:ceil(ih/2)*2"});
        } else if(ext == "webp" || ext == "apng") {
            args.insert(args.end(), {"-loop", "0"});
        }
        args.push_back(dest);
        auto r = run(args, timeoutMs);
        if(r.ok()) {
            result = dest;
            if(deleteOriginal) removeFile(file);
        } else {
            log("Ugoira conversion of `" + file + "` failed: " + trim(r.err), LogLevel::Error);
        }
    }
    for(auto& f : listDir(dir, false, true)) removeFile(buildPath(dir, f));
    g_rmdir(dir.c_str());
    return result;
}

std::string imageMagickConvert(const std::string& file, const std::string& ext, bool overwrite, bool deleteOriginal, int timeoutMs) {
    std::string dest = setExtension(file, ext);
    if(dest == file) return file;
    if(fileExists(dest) && !overwrite) return file;
    std::string program = available("magick") ? "magick" : "convert";
    auto r = run({program, file, dest}, timeoutMs);
    if(!r.ok()) {
        log("ImageMagick conversion of `" + file + "` failed: " + trim(r.err), LogLevel::Error);
        return file;
    }
    if(deleteOriginal) removeFile(file);
    return dest;
}

// ---- ZIP ---------------------------------------------------------------------

static uint32_t le32(const unsigned char* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t le16(const unsigned char* p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static bool inflateRaw(const std::string& in, std::string& out, size_t expected) {
    GZlibDecompressor* z = g_zlib_decompressor_new(G_ZLIB_COMPRESSOR_FORMAT_RAW);
    out.assign(expected, '\0');
    gsize read = 0, written = 0;
    GError* err = nullptr;
    GConverterResult res = g_converter_convert(G_CONVERTER(z), in.data(), in.size(), out.data(), out.size(), G_CONVERTER_INPUT_AT_END, &read, &written, &err);
    g_object_unref(z);
    if(err) g_error_free(err);
    out.resize(written);
    return res != G_CONVERTER_ERROR && written == expected;
}

bool unzip(const std::string& zipPath, const std::string& destDir, StringList* files) {
    auto data = readFile(zipPath);
    if(!data || data->size() < 22) return false;
    const auto* d = (const unsigned char*)data->data();
    size_t size = data->size();
    // End of central directory
    long eocd = -1;
    for(long i = (long)size - 22; i >= 0 && i >= (long)size - 65557; i--)
        if(le32(d + i) == 0x06054b50) {
            eocd = i;
            break;
        }
    if(eocd < 0) return false;
    uint16_t count = le16(d + eocd + 10);
    size_t cd = le32(d + eocd + 16);
    for(uint16_t n = 0; n < count; n++) {
        if(cd + 46 > size || le32(d + cd) != 0x02014b50) return false;
        uint16_t method = le16(d + cd + 10);
        uint32_t compSize = le32(d + cd + 20), uncompSize = le32(d + cd + 24);
        uint16_t nameLen = le16(d + cd + 28), extraLen = le16(d + cd + 30), commentLen = le16(d + cd + 32);
        uint32_t local = le32(d + cd + 42);
        std::string name((const char*)d + cd + 46, nameLen);
        cd += 46 + nameLen + extraLen + commentLen;
        if(name.empty() || endsWith(name, "/") || contains(name, "..")) continue;
        if(local + 30 > size || le32(d + local) != 0x04034b50) return false;
        size_t start = local + 30 + le16(d + local + 26) + le16(d + local + 28);
        if(start + compSize > size) return false;
        std::string comp((const char*)d + start, compSize), out;
        if(method == 0)
            out = comp;
        else if(method == 8) {
            if(!inflateRaw(comp, out, uncompSize)) return false;
        } else
            return false;
        std::string dest = buildPath(destDir, name);
        if(!writeFile(dest, out)) return false;
        if(files) files->push_back(name);
    }
    return true;
}

}  // namespace bg::external
