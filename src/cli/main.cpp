// booru-grabber-cli: command line interface, compatible with the options of Grabber-cli.
#include <glib.h>

#include <cstdio>
#include <cstring>
#include <iostream>

#include "config.hpp"
#include "core/download.hpp"
#include "core/logger.hpp"
#include "core/monitoring.hpp"
#include "core/paths.hpp"
#include "core/profile.hpp"

using namespace bg;

namespace {

struct CliOptions {
    gchar *tags = nullptr, *sources = nullptr, *location = nullptr, *filename = nullptr, *user = nullptr, *password = nullptr;
    gchar *tagsBlacklist = nullptr, *postfilter = nullptr, *tagsFormat = nullptr, *proxy = nullptr, *getDetails = nullptr;
    gchar *profile = nullptr, *importGrabber = nullptr;
    gint page = 1, max = -1, perpage = 20, tagsMin = 0;
    gboolean blacklist = FALSE, noDuplicates = FALSE, debug = FALSE, ignoreError = FALSE, noLogin = FALSE, json = FALSE;
    gboolean loadDetails = FALSE, loadTagDatabase = FALSE, returnCount = FALSE, returnTags = FALSE, returnPureTags = FALSE;
    gboolean returnImages = FALSE, download = FALSE, cli = FALSE, version = FALSE, portable = FALSE, listSites = FALSE;
};

GMainLoop* g_loop = nullptr;
int g_exit = 0;

void quit(int code) {
    g_exit = code;
    if(g_loop) g_main_loop_quit(g_loop);
}

// Grabber-cli used two-letter short options ("-tb", "-rc"...) that GOption cannot express
std::vector<std::string> normalizeArgs(int argc, char** argv) {
    static const std::map<std::string, std::string> aliases{
        {"-tb", "--tags-blacklist"}, {"-tm", "--tags-min"},         {"-tf", "--tags-format"},   {"-rc", "--return-count"},
        {"-rt", "--return-tags"},    {"-rp", "--return-pure-tags"}, {"-ri", "--return-images"}};
    std::vector<std::string> out;
    for(int i = 0; i < argc; i++) {
        auto it = aliases.find(argv[i]);
        out.push_back(it != aliases.end() ? it->second : argv[i]);
    }
    return out;
}

nlohmann::json imageJson(const ImagePtr& img) {
    nlohmann::json j;
    j["id"] = img->id();
    j["md5"] = img->md5();
    j["website"] = img->site() ? img->site()->url() : "";
    j["file_url"] = img->url();
    j["sample_url"] = img->url(SizeType::Sample);
    j["preview_url"] = img->url(SizeType::Thumbnail);
    j["page_url"] = img->pageUrl();
    j["width"] = img->width();
    j["height"] = img->height();
    j["file_size"] = img->fileSize();
    j["ext"] = img->extension();
    j["tags"] = img->tagNames();
    j["rating"] = tokenValue(img->tokens(), "rating").toString();
    j["score"] = tokenValue(img->tokens(), "score").toString();
    if(img->createdAt().valid()) j["created_at"] = img->createdAt().toIso();
    if(!img->sources().empty()) j["sources"] = img->sources();
    return j;
}

std::string formatTag(const std::string& fmt, const Tag& t) {
    std::string s = replaceAll(fmt, "\\t", "\t");
    s = replaceAll(s, "\\n", "\n");
    s = replaceAll(s, "%tag", t.text());
    s = replaceAll(s, "%count", std::to_string(t.count()));
    s = replaceAll(s, "%type", t.type().name());
    s = replaceAll(s, "%stype", std::to_string(t.type().number()));
    return s;
}

// Loads `max` images (or one page) from every site, then calls done
void collectImages(Profile& profile, const CliOptions& o, const std::vector<std::shared_ptr<Site>>& sites, const SearchQuery& query,
                   std::function<void(std::vector<ImagePtr>)> done) {
    struct State {
        size_t index = 0;
        std::vector<ImagePtr> images;
        std::shared_ptr<PackLoader> loader;
        std::set<std::string> seen;
    };
    auto st = std::make_shared<State>();
    auto next = std::make_shared<std::function<void()>>();
    *next = [&profile, &o, sites, query, st, next, done]() {
        if(st->index >= sites.size()) {
            done(st->images);
            return;
        }
        auto site = sites[st->index++];
        DownloadQueryGroup g;
        g.query = query;
        g.page = o.page;
        g.perpage = o.perpage;
        g.total = o.max > 0 ? o.max : o.perpage;
        g.postFiltering = o.postfilter ? split(o.postfilter, ' ', true) : StringList();
        g.site = site;
        st->loader = std::make_shared<PackLoader>(&profile, g, -1);
        st->loader->start();
        st->loader->next([st, next, &o](std::vector<ImagePtr> images) {
            for(auto& img : images) {
                std::string key = !img->md5().empty() ? img->md5() : img->url();
                if(o.noDuplicates && !st->seen.insert(key).second) continue;
                st->images.push_back(img);
            }
            (*next)();
        });
    };
    (*next)();
}

void loadAllDetails(std::vector<ImagePtr> images, std::function<void()> done) {
    auto st = std::make_shared<std::pair<std::vector<ImagePtr>, size_t>>(std::move(images), 0);
    auto next = std::make_shared<std::function<void()>>();
    *next = [st, next, done]() {
        if(st->second >= st->first.size()) {
            done();
            return;
        }
        auto img = st->first[st->second++];
        img->loadDetails([next](Image::LoadResult) { (*next)(); });
    };
    (*next)();
}

}  // namespace

int main(int argc, char** argv) {
    g_set_prgname("booru-grabber-cli");
    CliOptions o;
    GOptionEntry entries[] = {
        {"tags", 't', 0, G_OPTION_ARG_STRING, &o.tags, "Tags to search for", "TAGS"},
        {"sources", 's', 0, G_OPTION_ARG_STRING, &o.sources, "Source websites (space or comma separated)", "SITES"},
        {"page", 'p', 0, G_OPTION_ARG_INT, &o.page, "Starting page (default 1)", "N"},
        {"max", 'm', 0, G_OPTION_ARG_INT, &o.max, "Maximum number of returned images", "N"},
        {"perpage", 'i', 0, G_OPTION_ARG_INT, &o.perpage, "Images per page (default 20)", "N"},
        {"location", 'l', 0, G_OPTION_ARG_FILENAME, &o.location, "Folder to save images in (default: Save/path setting)", "PATH"},
        {"filename", 'f', 0, G_OPTION_ARG_STRING, &o.filename, "Filename format (default: Save/filename setting)", "FORMAT"},
        {"user", 'u', 0, G_OPTION_ARG_STRING, &o.user, "Username to log into the sources", "USER"},
        {"password", 'w', 0, G_OPTION_ARG_STRING, &o.password, "Password to log into the sources", "PASSWORD"},
        {"blacklist", 'b', 0, G_OPTION_ARG_NONE, &o.blacklist, "Download blacklisted images", nullptr},
        {"tags-blacklist", 0, 0, G_OPTION_ARG_STRING, &o.tagsBlacklist, "Tags to remove from results (-tb)", "TAGS"},
        {"postfilter", 'r', 0, G_OPTION_ARG_STRING, &o.postfilter, "Filter results", "FILTER"},
        {"no-duplicates", 'n', 0, G_OPTION_ARG_NONE, &o.noDuplicates, "Remove duplicates from results", nullptr},
        {"debug", 'd', 0, G_OPTION_ARG_NONE, &o.debug, "Show debug messages", nullptr},
        {"tags-min", 0, 0, G_OPTION_ARG_INT, &o.tagsMin, "Minimum count for tags to be returned (-tm)", "N"},
        {"tags-format", 0, 0, G_OPTION_ARG_STRING, &o.tagsFormat, "Format for returned tags (-tf, default \"%tag\\t%count\\t%type\")", "FORMAT"},
        {"ignore-error", 0, 0, G_OPTION_ARG_NONE, &o.ignoreError, "Don't exit on error", nullptr},
        {"proxy", 0, 0, G_OPTION_ARG_STRING, &o.proxy, "Use the given proxy", "[user:password@]host:port"},
        {"no-login", 0, 0, G_OPTION_ARG_NONE, &o.noLogin, "Disable auto login", nullptr},
        {"json", 'j', 0, G_OPTION_ARG_NONE, &o.json, "Output results as JSON", nullptr},
        {"load-details", 0, 0, G_OPTION_ARG_NONE, &o.loadDetails, "Load the details page of each result", nullptr},
        {"get-details", 0, 0, G_OPTION_ARG_STRING, &o.getDetails, "Parse the details of the given image page", "URL"},
        {"load-tag-database", 0, 0, G_OPTION_ARG_NONE, &o.loadTagDatabase, "Load the tag database of the given sources", nullptr},
        {"return-count", 0, 0, G_OPTION_ARG_NONE, &o.returnCount, "Return the total image count (-rc)", nullptr},
        {"return-tags", 0, 0, G_OPTION_ARG_NONE, &o.returnTags, "Return the tags of a search (-rt)", nullptr},
        {"return-pure-tags", 0, 0, G_OPTION_ARG_NONE, &o.returnPureTags, "Return the tag list of the sources (-rp)", nullptr},
        {"return-images", 0, 0, G_OPTION_ARG_NONE, &o.returnImages, "Return the found images (-ri)", nullptr},
        {"download", 0, 0, G_OPTION_ARG_NONE, &o.download, "Download the found images", nullptr},
        {"list-sites", 0, 0, G_OPTION_ARG_NONE, &o.listSites, "List the configured sites and their sources", nullptr},
        {"cli", 'c', G_OPTION_FLAG_HIDDEN, G_OPTION_ARG_NONE, &o.cli, "Ignored (compatibility)", nullptr},
        {"profile", 0, 0, G_OPTION_ARG_FILENAME, &o.profile, "Keep all settings and data in this directory", "DIR"},
        {"portable", 0, 0, G_OPTION_ARG_NONE, &o.portable, "Keep settings and data next to the program", nullptr},
        {"import-grabber", 0, 0, G_OPTION_ARG_FILENAME, &o.importGrabber, "Import a profile of the original Grabber (e.g. ~/.config/Bionus/Grabber) and exit", "DIR"},
        {"version", 'v', 0, G_OPTION_ARG_NONE, &o.version, "Show the version", nullptr},
        {nullptr, 0, 0, G_OPTION_ARG_NONE, nullptr, nullptr, nullptr}};

    std::vector<std::string> args = normalizeArgs(argc, argv);
    std::vector<char*> cargs;
    for(auto& a : args) cargs.push_back(a.data());
    int cargc = (int)cargs.size();
    char** cargv = cargs.data();

    GOptionContext* ctx = g_option_context_new("- download images from boorus and imageboards");
    g_option_context_add_main_entries(ctx, entries, nullptr);
    g_option_context_set_summary(ctx, "Searches, lists and downloads images from the sites configured in Booru Grabber.");
    g_option_context_set_description(ctx, "Example:\n  booru-grabber-cli -t \"cat_ears rating:general\" -s safebooru.org -m 50 --download\n");
    GError* err = nullptr;
    if(!g_option_context_parse(ctx, &cargc, &cargv, &err)) {
        fprintf(stderr, "booru-grabber-cli: %s\n", err->message);
        g_error_free(err);
        return 2;
    }
    g_option_context_free(ctx);
    if(o.version) {
        printf("booru-grabber-cli %s\n", BG_VERSION);
        return 0;
    }

    paths::init(o.profile ? o.profile : "", o.portable);
    Logger& logger = Logger::instance();
    logger.setConsoleLevel(o.debug ? LogLevel::Debug : LogLevel::Warning);
    logger.setLogFile(buildPath(paths::stateDir(), "logs/cli.log"));

    if(o.importGrabber) {
        int n = importGrabberProfile(o.importGrabber);
        if(n < 0) {
            fprintf(stderr, "No Grabber profile (settings.ini) found in %s\n", o.importGrabber);
            return 1;
        }
        printf("Imported %d files from %s into %s and %s\n", n, o.importGrabber, paths::configDir().c_str(), paths::dataDir().c_str());
        return 0;
    }

    Profile profile;
    importLegacyCookies();
    Settings& s = profile.settings();

    if(o.listSites) {
        for(auto& [url, site] : profile.sites()) printf("%s\t%s\n", url.c_str(), site->type().c_str());
        return 0;
    }

    if(o.proxy) {
        std::string p = o.proxy;
        std::string auth, host = p;
        size_t at = p.rfind('@');
        if(at != std::string::npos) {
            auth = p.substr(0, at);
            host = p.substr(at + 1);
        }
        s.setBool("Proxy/use", true);
        s.setBool("Proxy/useSystem", false);
        size_t colon = host.rfind(':');
        s.set("Proxy/hostName", colon == std::string::npos ? host : host.substr(0, colon));
        s.setInt("Proxy/port", colon == std::string::npos ? 8080 : toInt(host.substr(colon + 1), 8080));
        size_t c2 = auth.find(':');
        s.set("Proxy/user", c2 == std::string::npos ? auth : auth.substr(0, c2));
        s.set("Proxy/password", c2 == std::string::npos ? "" : auth.substr(c2 + 1));
        net::Session::instance().applySettings(s);
    }
    if(o.tagsBlacklist) profile.removedTags().add(split(o.tagsBlacklist, ' ', true));

    StringList siteNames = o.sources ? splitAny(o.sources, " ,", true) : s.getList("sites");
    auto sites = profile.filteredSites(siteNames);
    for(auto& site : sites) {
        if(o.noLogin) site->setAutoLogin(false);
        if(o.user) site->settings().set("auth/pseudo", o.user);
        if(o.password) site->settings().set("auth/password", o.password);
    }

    SearchQuery query;
    query.tags = o.tags ? split(o.tags, ' ', true) : StringList();
    std::string filename = o.filename ? o.filename : s.get("Save/filename");
    std::string location = o.location ? expandHome(o.location) : profile.savePath();
    std::string tagsFormat = o.tagsFormat ? o.tagsFormat : "%tag\\t%count\\t%type";

    g_loop = g_main_loop_new(nullptr, FALSE);
    bool started = false;
    std::vector<std::shared_ptr<void>> keepAlive;  // pages, loaders and replies of the running command

    if(o.getDetails) {
        started = true;
        std::string url = o.getDetails;
        std::shared_ptr<Site> site;
        for(auto& [u, st] : profile.sites())
            if(contains(url, u)) site = st;
        // A page URL is HTML: prefer the HTML API's parser
        Api* api = site ? site->source()->api("Html") : nullptr;
        if(api && !api->canLoadDetails()) api = nullptr;
        if(!api && site) api = site->detailsApi();
        if(!site || !api) {
            fprintf(stderr, "No site with a details API found for %s\n", url.c_str());
            return 1;
        }
        keepAlive.push_back(site->get(url, QueryType::Details, [site, api, &o](const net::Response& r) {
            ParsedDetails d = api->parseDetails(r.body, r.status, site->info());
            if(!d.error.empty()) {
                fprintf(stderr, "%s\n", d.error.c_str());
                quit(1);
                return;
            }
            if(d.image) {
                Image img(site, *d.image, site->profile());
                d.tags = img.tags();
                d.imageUrl = img.url();
                d.createdAt = img.createdAt();
            }
            nlohmann::json j;
            StringList tags;
            for(auto& t : d.tags) tags.push_back(t.text());
            j["tags"] = tags;
            j["imageUrl"] = d.imageUrl;
            if(d.createdAt.valid()) j["createdAt"] = d.createdAt.toIso();
            j["sources"] = d.sources;
            if(o.json)
                printf("%s\n", j.dump(4).c_str());
            else {
                printf("Image URL: %s\n", d.imageUrl.c_str());
                printf("Tags: %s\n", join(tags, " ").c_str());
                if(d.createdAt.valid()) printf("Date: %s\n", d.createdAt.toIso().c_str());
            }
            quit(0);
        }));
    } else if(o.loadTagDatabase) {
        started = true;
        auto index = std::make_shared<size_t>(0);
        auto next = std::make_shared<std::function<void()>>();
        auto loaders = std::make_shared<std::vector<std::shared_ptr<TagListLoader>>>();
        *next = [sites, index, next, loaders, &o]() {
            if(*index >= sites.size()) {
                quit(0);
                return;
            }
            auto site = sites[(*index)++];
            auto loader = std::make_shared<TagListLoader>(site, o.tagsMin);
            loaders->push_back(loader);
            loader->progress.connect([site](int pages, int count) {
                fprintf(stderr, "\r[%s] %d pages, %d tags", site->url().c_str(), pages, count);
            });
            loader->start([next, site](bool ok, int count) {
                fprintf(stderr, "\n[%s] %s: %d tags\n", site->url().c_str(), ok ? "done" : "error", count);
                (*next)();
            });
        };
        (*next)();
    } else if(o.returnCount || o.returnTags) {
        started = true;
        auto remaining = std::make_shared<size_t>(sites.size());
        auto total = std::make_shared<long long>(0);
        auto tags = std::make_shared<std::map<std::string, Tag>>();
        if(sites.empty()) quit(1);
        for(auto& site : sites) {
            std::vector<Site*> raw;
            for(auto& s2 : sites) raw.push_back(s2.get());
            auto page = std::make_shared<Page>(&profile, site, raw, query, o.page, o.perpage, o.postfilter ? split(o.postfilter, ' ', true) : StringList());
            keepAlive.push_back(page);
            page->load([remaining, total, tags, &o, tagsFormat](Page* p, bool ok) {
                if(ok) {
                    int c = p->imagesCount();
                    if(c > 0) *total += c;
                    for(auto& t : p->tags()) {
                        auto it = tags->find(t.text());
                        if(it == tags->end())
                            (*tags)[t.text()] = t;
                        else
                            it->second.setCount(std::max(it->second.count(), t.count()));
                    }
                } else if(!o.ignoreError) {
                    for(auto& e : p->errors()) fprintf(stderr, "[%s] %s\n", p->site()->url().c_str(), e.c_str());
                }
                if(--*remaining > 0) return;
                if(o.returnCount) {
                    if(o.json)
                        printf("%s\n", nlohmann::json{{"count", *total}}.dump().c_str());
                    else
                        printf("%lld\n", *total);
                } else {
                    TagList list;
                    for(auto& [_, t] : *tags)
                        if(t.count() >= o.tagsMin) list.push_back(t);
                    sortTags(list, "count");
                    if(o.json) {
                        nlohmann::json a = nlohmann::json::array();
                        for(auto& t : list) a.push_back(t.toJson());
                        printf("%s\n", a.dump(4).c_str());
                    } else {
                        for(auto& t : list) printf("%s\n", formatTag(tagsFormat, t).c_str());
                    }
                }
                quit(0);
            });
        }
    } else if(o.returnPureTags) {
        started = true;
        auto remaining = std::make_shared<size_t>(sites.size());
        auto all = std::make_shared<TagList>();
        if(sites.empty()) quit(1);
        for(auto& site : sites) {
            Api* api = site->tagsApi();
            if(!api) {
                fprintf(stderr, "[%s] This site cannot list tags\n", site->url().c_str());
                if(--*remaining == 0) quit(1);
                continue;
            }
            PageUrl url = api->tagsUrl(o.page, o.perpage, "count", site->info());
            keepAlive.push_back(site->get(url.url, QueryType::List, [remaining, all, site, api, &o, tagsFormat](const net::Response& r) {
                ParsedTags t = api->parseTags(r.body, r.status, site->info());
                for(auto& tag : t.tags)
                    if(tag.count() >= o.tagsMin) all->push_back(tag);
                if(--*remaining > 0) return;
                if(o.json) {
                    nlohmann::json a = nlohmann::json::array();
                    for(auto& tag : *all) a.push_back(tag.toJson());
                    printf("%s\n", a.dump(4).c_str());
                } else {
                    for(auto& tag : *all) printf("%s\n", formatTag(tagsFormat, tag).c_str());
                }
                quit(0);
            }));
        }
    } else if(o.returnImages || o.download) {
        started = true;
        if(sites.empty()) {
            fprintf(stderr, "No valid site given (use -s or configure default sites)\n");
            return 1;
        }
        auto batches = std::make_shared<std::vector<std::shared_ptr<BatchDownloader>>>();
        collectImages(profile, o, sites, query, [&profile, &o, filename, location, batches](std::vector<ImagePtr> images) {
            auto output = [&o, images, &profile, filename, location, batches]() {
                if(o.returnImages) {
                    if(o.json) {
                        nlohmann::json a = nlohmann::json::array();
                        for(auto& img : images) a.push_back(imageJson(img));
                        printf("%s\n", a.dump(4).c_str());
                    } else {
                        for(auto& img : images) printf("%s\n", img->url().c_str());
                    }
                }
                if(!o.download) {
                    quit(images.empty() && !o.ignoreError ? 1 : 0);
                    return;
                }
                if(images.empty()) {
                    fprintf(stderr, "No images found\n");
                    quit(o.ignoreError ? 0 : 1);
                    return;
                }
                std::vector<DownloadQueryImage> queries;
                for(auto& img : images) {
                    DownloadQueryImage q;
                    q.image = img;
                    q.site = img->sitePtr();
                    q.filename = filename;
                    q.path = location;
                    queries.push_back(q);
                }
                auto batch = std::make_shared<BatchDownloader>(&profile, queries);
                batches->push_back(batch);
                size_t total = images.size();
                std::weak_ptr<BatchDownloader> weak = batch;
                batch->imageSaved.connect([weak, total](const ImagePtr& img, const SaveResults& r) {
                    auto b = weak.lock();
                    if(!b) return;
                    std::string what = r.empty() ? "error" : saveResultName(r[0].result);
                    std::string path = r.empty() ? "" : r[0].path;
                    fprintf(stderr, "[%d/%zu] %s: %s\n", b->processed(), total, what.c_str(), path.empty() ? img->url().c_str() : path.c_str());
                });
                batch->finished.connect([weak, &o] {
                    auto b = weak.lock();
                    if(!b) return;
                    fprintf(stderr, "Downloaded %d, already existed %d, ignored %d, not found %d, errors %d\n", b->counter(BatchDownloader::Downloaded),
                            b->counter(BatchDownloader::AlreadyExists), b->counter(BatchDownloader::Ignored), b->counter(BatchDownloader::NotFound),
                            b->counter(BatchDownloader::Errors));
                    quit(b->counter(BatchDownloader::Errors) > 0 && !o.ignoreError ? 1 : 0);
                });
                batch->setGetBlacklisted(o.blacklist);
                batch->start();
            };
            if(o.loadDetails)
                loadAllDetails(images, output);
            else
                output();
        });
    }

    if(!started) {
        fprintf(stderr, "Nothing to do. Use --return-count, --return-tags, --return-pure-tags, --return-images, --download, "
                        "--get-details or --load-tag-database (see --help).\n");
        g_main_loop_unref(g_loop);
        return 2;
    }
    g_main_loop_run(g_loop);
    g_main_loop_unref(g_loop);
    return g_exit;
}
